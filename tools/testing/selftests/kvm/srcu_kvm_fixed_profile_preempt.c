#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/sizes.h>
#include "kvm_util.h"
#include "processor.h"
#include "ucall_common.h"
#define TEST_SLOT 1
#define TEST_NPAGES 2
#define TEST_MEM_GPA 0x400000ULL
#define TEST_IOEVENT_ADDR (4ull * SZ_1G)
#define TEST_IOEVENT_LEN 4
#define FIXED_RELOADS 64
#define FIXED_SPIN_DURATION_MS 8000ULL
#define FIXED_GP_SETTLE_US 5000ULL
#define FIXED_IOEVENTFD_INTERVAL_US 200ULL
#define FIXED_PRE_BURST 8
#define PREEMPT_CPU 0
#define PREEMPT_FIFO_PRIO 50
#define PREEMPT_BUSY_US 800U
#define PREEMPT_SLEEP_US 50U
static volatile uint64_t guest_stop, guest_spin_duration_ms;
static struct kvm_vm *g_vm;
static struct kvm_vcpu *g_vcpu;
static bool g_use_user_memory2, g_preempt_enable;
static volatile bool g_preempt_stop;
static void bind_self_to_cpu(int cpu, const char *who)
{
	cpu_set_t set;
	int ar;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	ar = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	TEST_ASSERT(!ar, "pthread_setaffinity_np(%s) failed: cpu=%d err=%d", who, cpu, ar);
}
static void ioeventfd_update(struct kvm_vm *vm, uint64_t addr, int efd, bool deassign)
{
	struct kvm_ioeventfd io = { .datamatch = 0, .addr = addr, .len = TEST_IOEVENT_LEN, .fd = efd,
				    .flags = deassign ? KVM_IOEVENTFD_FLAG_DEASSIGN : 0 };
	int ret = __vm_ioctl(vm, KVM_IOEVENTFD, &io);
	TEST_ASSERT(!ret, "KVM_IOEVENTFD %s failed, ret=%d errno=%d (%s)",
		    deassign ? "deassign" : "assign", ret, errno, strerror(errno));
}
static void set_memslot(struct kvm_vm *vm, uint64_t size, void *hva)
{
	int ret = g_use_user_memory2 ?
		  __vm_set_user_memory_region2(vm, TEST_SLOT, 0, TEST_MEM_GPA, size, hva, 0, 0) :
		  __vm_set_user_memory_region(vm, TEST_SLOT, 0, TEST_MEM_GPA, size, hva);
	TEST_ASSERT(!ret, "KVM_SET_USER_MEMORY_REGION%s failed, ret=%d errno=%d (%s)",
		    g_use_user_memory2 ? "2" : "", ret, errno, strerror(errno));
}
static void guest_main(void)
{
	uint64_t start, now, target_cycles;
	/* 中文注释：让 guest 在 SRCU 读侧窗口里忙等较长时间。 */
	GUEST_SYNC(0);
	GUEST_ASSERT(guest_tsc_khz);
	target_cycles = guest_tsc_khz * guest_spin_duration_ms;
	start = rdtsc();
	while (!guest_stop) {
		now = rdtsc();
		if (now - start >= target_cycles)
			break;
		asm volatile("pause" ::: "memory");
	}
	GUEST_DONE();
}
static void *vcpu_thread_main(void *arg)
{
	struct ucall uc;
	(void)arg;
	if (g_preempt_enable)
		bind_self_to_cpu(PREEMPT_CPU, "vcpu");
	for (;;) {
		vcpu_run(g_vcpu);
		switch (get_ucall(g_vcpu, &uc)) {
		case UCALL_SYNC:
		case UCALL_DONE:
			return NULL;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			return NULL;
		default:
			TEST_ASSERT(false, "Unexpected ucall=%ld exit_reason=%u", uc.cmd,
				    g_vcpu->run->exit_reason);
		}
	}
}
static void *preempt_thread_main(void *arg)
{
	struct sched_param sp = { .sched_priority = PREEMPT_FIFO_PRIO };
	struct timespec t0, t1;
	int ar;
	(void)arg;
	/* 中文注释：抢占线程与主线程/vcpu 同核，周期性忙等制造切出。 */
	bind_self_to_cpu(PREEMPT_CPU, "preempt");
	ar = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	if (ar)
		pr_info("preempt thread: pthread_setschedparam(SCHED_FIFO,%d) failed: %d\n",
			PREEMPT_FIFO_PRIO, ar);
	while (!g_preempt_stop) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			uint64_t ns0, ns1;
			if (g_preempt_stop)
				break;
			asm volatile("pause" ::: "memory");
			clock_gettime(CLOCK_MONOTONIC, &t1);
			ns0 = (uint64_t)t0.tv_sec * 1000000000ULL + (uint64_t)t0.tv_nsec;
			ns1 = (uint64_t)t1.tv_sec * 1000000000ULL + (uint64_t)t1.tv_nsec;
			if (ns1 - ns0 >= (uint64_t)PREEMPT_BUSY_US * 1000ULL)
				break;
		}
		if (!g_preempt_stop && PREEMPT_SLEEP_US)
			usleep(PREEMPT_SLEEP_US);
	}
	return NULL;
}
int main(int argc, char *argv[])
{
	pthread_t vcpu_thread, preempt_thread;
	size_t mem_size;
	void *mem;
	int eventfd, ret;
	if (argc != 2 || (strcmp(argv[1], "0") && strcmp(argv[1], "1"))) {
		pr_info("Usage: %s <storm>\n", argv[0]);
		pr_info("  storm: 0|1 (compat only, now ignored)\n");
		return 2;
	}
	g_preempt_enable = getenv("PREEMPT_SIM") && strcmp(getenv("PREEMPT_SIM"), "0");
	if (g_preempt_enable) {
		/* 中文注释：主线程绑核，保证 memslot reload 也在同核竞争域。 */
		bind_self_to_cpu(PREEMPT_CPU, "main");
		pr_info("preempt sim enabled: cpu=%d fifo_prio=%d busy_us=%u sleep_us=%u\n",
			PREEMPT_CPU, PREEMPT_FIFO_PRIO, PREEMPT_BUSY_US, PREEMPT_SLEEP_US);
	}
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_IOEVENTFD));
	g_use_user_memory2 = kvm_has_cap(KVM_CAP_USER_MEMORY2);
	g_vm = vm_create_with_one_vcpu(&g_vcpu, guest_main);
	mem_size = TEST_NPAGES * g_vm->page_size;
	mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap() failed errno=%d (%s)", errno, strerror(errno));
	set_memslot(g_vm, mem_size, mem);
	guest_stop = 0;
	guest_spin_duration_ms = FIXED_SPIN_DURATION_MS;
	sync_global_to_guest(g_vm, guest_stop);
	sync_global_to_guest(g_vm, guest_spin_duration_ms);
	ret = pthread_create(&vcpu_thread, NULL, vcpu_thread_main, NULL);
	TEST_ASSERT(!ret, "pthread_create(vcpu_thread sync) failed: %d", ret);
	pthread_join(vcpu_thread, NULL);
	ret = pthread_create(&vcpu_thread, NULL, vcpu_thread_main, NULL);
	TEST_ASSERT(!ret, "pthread_create(vcpu_thread loop) failed: %d", ret);
	eventfd = kvm_new_eventfd();
	for (uint32_t i = 0; i < FIXED_PRE_BURST; i++) {
		ioeventfd_update(g_vm, TEST_IOEVENT_ADDR, eventfd, false);
		if (i + 1 < FIXED_PRE_BURST) {
			ioeventfd_update(g_vm, TEST_IOEVENT_ADDR, eventfd, true);
			usleep(FIXED_IOEVENTFD_INTERVAL_US);
		}
	}
	usleep(FIXED_GP_SETTLE_US);
	if (g_preempt_enable) {
		g_preempt_stop = false;
		ret = pthread_create(&preempt_thread, NULL, preempt_thread_main, NULL);
		TEST_ASSERT(!ret, "pthread_create(preempt_thread) failed: %d", ret);
	}
	for (uint32_t i = 0; i < FIXED_RELOADS; i++) {
		set_memslot(g_vm, 0, mem);
		set_memslot(g_vm, mem_size, mem);
	}
	if (g_preempt_enable) {
		g_preempt_stop = true;
		pthread_join(preempt_thread, NULL);
	}
	guest_stop = 1;
	sync_global_to_guest(g_vm, guest_stop);
	pthread_join(vcpu_thread, NULL);
	kvm_vm_free(g_vm);
	munmap(mem, mem_size);
	close(eventfd);
	return 0;
}
