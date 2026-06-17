#include <uapi/linux/dsa_sl.h>
#include "dsa.h"
#include "mm.h"

#include "linux/sched.h"
#include <linux/kthread.h>

static int thread_function(void *args)
{
	struct dsa_memcpy_reqs *reqs = (struct dsa_memcpy_reqs *)args;
	// TODO
	while (!kthread_should_stop()) {
		struct dsa_memcpy_args *req = dequeue_nolock(reqs);
		if (req) {
			pr_info("Thread: src = %llx, dst = %llx, size = %llx\n",
				req->src, req->dst, req->size);
		}
		schedule();
	}

	return 0;
}

int dsa_start_thread(struct dsa_device_info *dsa, struct dsa_memcpy_reqs *reqs)
{
	char th_name[] = "dsa_thread";
	dsa->kth = kthread_create(thread_function, reqs, th_name);

	if (dsa->kth == NULL) {
		pr_err("Thread creation failed\n");
		return -1;
	}

	wake_up_process(dsa->kth);
	pr_info("Thread created successfully\n");
	return 0;
}

int dsa_stop_thread(struct dsa_device_info *dsa)
{
	int ret;
	if (dsa->kth == NULL) {
		pr_err("Thread is not running\n");
		return 0;
	}

	ret = kthread_stop(dsa->kth);
	if (ret != 0) {
		if (ret == -EINTR) {
			pr_err("wake_up_process() is never called!\n");
		} else {
			pr_err("Thread has error when stopping!\n");
		}
		return 0;
	}
	pr_info("Thread stopped\n");
	return 0;
}
