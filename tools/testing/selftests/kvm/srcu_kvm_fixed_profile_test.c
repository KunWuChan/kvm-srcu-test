#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/sizes.h>
#include "kvm_util.h"
#include "processor.h"
#include "ucall_common.h"

#define TEST_SLOT                1
#define TEST_NPAGES              2
#define TEST_MEM_GPA             0x400000ULL
#define TEST_IOEVENT_ADDR        (4ull * SZ_1G)
#define TEST_IOEVENT_LEN         4
#define STORM_IOEVENT_ADDR       (TEST_IOEVENT_ADDR + 0x1000ULL)
/* Fixed profile tuned for stable nonbaseline reproduction. */
#define FIXED_RELOADS            64
#define FIXED_SPIN_DURATION_MS   8000ULL
#define FIXED_GP_SETTLE_US       5000ULL
#define FIXED_IOEVENTFD_INTERVAL_US 200ULL
#define FIXED_PRE_BURST          8
#define FIXED_STORM_ROUNDS       12000
#define FIXED_STORM_INTERVAL_US  0ULL

static volatile uint64_t guest_stop;
static volatile uint64_t guest_spin_duration_ms;
static struct kvm_vm *g_vm;
static struct kvm_vcpu *g_vcpu;
static bool g_use_user_memory2;
static volatile bool g_storm_stop;
static int g_storm_eventfd = -1;

static void ioeventfd_update(struct kvm_vm *vm, uint64_t addr, uint32_t len, int efd, bool deassign)
{
	struct kvm_ioeventfd io = { .datamatch = 0, .addr = addr, .len = len, .fd = efd,
				    .flags = deassign ? KVM_IOEVENTFD_FLAG_DEASSIGN : 0 };
	int ret = __vm_ioctl(vm, KVM_IOEVENTFD, &io);
	TEST_ASSERT(!ret, "KVM_IOEVENTFD %s failed, ret=%d errno=%d (%s)",
		    deassign ? "deassign" : "assign", ret, errno, strerror(errno));
}

static void set_memslot(struct kvm_vm *vm, bool use_user_memory2, uint32_t slot,
			uint64_t gpa, uint64_t size, void *hva)
{
	int ret;

	if (use_user_memory2)
		ret = __vm_set_user_memory_region2(vm, slot, 0, gpa, size, hva, 0, 0);
	else
		ret = __vm_set_user_memory_region(vm, slot, 0, gpa, size, hva);

	TEST_ASSERT(!ret, "KVM_SET_USER_MEMORY_REGION%s failed, ret=%d errno=%d (%s)",
		    use_user_memory2 ? "2" : "", ret, errno, strerror(errno));
}

static void guest_main(void)
{
	uint64_t start, now, target_cycles;

	/* Guest enters a long read-side busy window. */
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
	for (;;) {
		vcpu_run(g_vcpu);
		switch (get_ucall(g_vcpu, &uc)) {
		case UCALL_SYNC:
			return NULL;
		case UCALL_DONE:
			return NULL;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			return NULL;
		default:
			TEST_ASSERT(false, "Unexpected ucall=%ld exit_reason=%u",
				    uc.cmd, g_vcpu->run->exit_reason);
		}
	}
}

static void *io_storm_thread_main(void *arg)
{
	uint32_t i = 0;
	struct kvm_ioeventfd io = {
		.datamatch = 0,
		.addr = STORM_IOEVENT_ADDR,
		.len = TEST_IOEVENT_LEN,
		.fd = 0,
		.flags = 0,
	};

	(void)arg;
	/* Keep adding normal-side pressure while reload loop runs. */
	io.fd = g_storm_eventfd;
	while (!g_storm_stop && i < FIXED_STORM_ROUNDS) {
		io.flags = 0;
		(void)__vm_ioctl(g_vm, KVM_IOEVENTFD, &io);
		io.flags = KVM_IOEVENTFD_FLAG_DEASSIGN;
		(void)__vm_ioctl(g_vm, KVM_IOEVENTFD, &io);
		i++;
		if (FIXED_STORM_INTERVAL_US)
			usleep(FIXED_STORM_INTERVAL_US);
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t vcpu_thread, storm_thread;
	bool storm_enabled = false;
	size_t mem_size;
	void *mem;
	int eventfd = -1;
	int ret;

	if (argc != 2 || (strcmp(argv[1], "0") && strcmp(argv[1], "1"))) {
		pr_info("Usage: %s <storm>\n", argv[0]);
		pr_info("  storm: 0|1\n");
		return 2;
	}
	storm_enabled = !strcmp(argv[1], "1");

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_IOEVENTFD));
	g_use_user_memory2 = kvm_has_cap(KVM_CAP_USER_MEMORY2);
	g_vm = vm_create_with_one_vcpu(&g_vcpu, guest_main);
	mem_size = TEST_NPAGES * g_vm->page_size;
	mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT(mem != MAP_FAILED, "mmap() failed errno=%d (%s)", errno, strerror(errno));
	set_memslot(g_vm, g_use_user_memory2, TEST_SLOT, TEST_MEM_GPA, mem_size, mem);

	guest_stop = 0;
	guest_spin_duration_ms = FIXED_SPIN_DURATION_MS;
	sync_global_to_guest(g_vm, guest_stop);
	sync_global_to_guest(g_vm, guest_spin_duration_ms);
	ret = pthread_create(&vcpu_thread, NULL, vcpu_thread_main, NULL);
	TEST_ASSERT(!ret, "pthread_create(vcpu_thread) failed: %d", ret);
	pthread_join(vcpu_thread, NULL);

	ret = pthread_create(&vcpu_thread, NULL, vcpu_thread_main, NULL);
	TEST_ASSERT(!ret, "pthread_create(vcpu_thread loop) failed: %d", ret);

	eventfd = kvm_new_eventfd();
	for (uint32_t i = 0; i < FIXED_PRE_BURST; i++) {
		ioeventfd_update(g_vm, TEST_IOEVENT_ADDR, TEST_IOEVENT_LEN, eventfd, false);
		if (i + 1 < FIXED_PRE_BURST) {
			ioeventfd_update(g_vm, TEST_IOEVENT_ADDR, TEST_IOEVENT_LEN, eventfd, true);
			usleep(FIXED_IOEVENTFD_INTERVAL_US);
		}
	}

	if (FIXED_GP_SETTLE_US)
		usleep(FIXED_GP_SETTLE_US);

	if (storm_enabled) {
		g_storm_stop = false;
		g_storm_eventfd = kvm_new_eventfd();
		ret = pthread_create(&storm_thread, NULL, io_storm_thread_main, NULL);
		TEST_ASSERT(!ret, "pthread_create(storm_thread) failed: %d", ret);
	}

	for (uint32_t i = 0; i < FIXED_RELOADS; i++) {
		set_memslot(g_vm, g_use_user_memory2, TEST_SLOT, TEST_MEM_GPA, 0, mem);
		set_memslot(g_vm, g_use_user_memory2, TEST_SLOT, TEST_MEM_GPA, mem_size, mem);
	}

	if (storm_enabled) {
		g_storm_stop = true;
		pthread_join(storm_thread, NULL);
	}

	guest_stop = 1;
	sync_global_to_guest(g_vm, guest_stop);
	pthread_join(vcpu_thread, NULL);

	kvm_vm_free(g_vm);
	munmap(mem, mem_size);
	if (eventfd >= 0)
		close(eventfd);
	if (g_storm_eventfd >= 0)
		close(g_storm_eventfd);
	return 0;
}
