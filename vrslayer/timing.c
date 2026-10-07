#include "layer.h"

#define STAMPS            2
#define START_QUERY(slot) (STAMPS * (slot))
#define END_QUERY(slot)   (STAMPS * (slot) + 1)
#define SCRATCH_BYTES     256
#define MAX_FRAME_NS      1e9f

/* Query value plus availability. */
struct stamp {
	uint64_t value;
	uint64_t available;
};

static const VkCommandBufferBeginInfo reusable = {
	.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
};

/* Stamp follows acquire-waiting fill. */
static int
record_begin(struct device *dev, uint32_t slot)
{
	VkCommandBuffer cmd = dev->timing.begin[slot];

	if (dev->vk.BeginCommandBuffer(cmd, &reusable) != VK_SUCCESS)
		return 0;
	dev->vk.CmdResetQueryPool(cmd, dev->queries, START_QUERY(slot), STAMPS);
	dev->vk.CmdFillBuffer(cmd, dev->timing.scratch, 0, sizeof(uint32_t), 0);
	dev->vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		dev->queries, START_QUERY(slot));
	return dev->vk.EndCommandBuffer(cmd) == VK_SUCCESS;
}

static int
record_end(struct device *dev, uint32_t slot)
{
	VkCommandBuffer cmd = dev->timing.end[slot];

	if (dev->vk.BeginCommandBuffer(cmd, &reusable) != VK_SUCCESS)
		return 0;
	dev->vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		dev->queries, END_QUERY(slot));
	return dev->vk.EndCommandBuffer(cmd) == VK_SUCCESS;
}

static int
create_scratch(struct device *dev)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = SCRATCH_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	struct timing *t = &dev->timing;
	VkMemoryRequirements req;

	if (dev->vk.CreateBuffer(dev->handle, &info, NULL, &t->scratch) != VK_SUCCESS)
		return 0;
	dev->vk.GetBufferMemoryRequirements(dev->handle, t->scratch, &req);
	t->scratch_memory = device_allocate(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	return t->scratch_memory && dev->vk.BindBufferMemory(dev->handle, t->scratch,
		t->scratch_memory, 0) == VK_SUCCESS;
}

int
timing_init(struct device *dev)
{
	VkSemaphoreCreateInfo binary = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	uint32_t slot;

	if (!create_scratch(dev))
		return 0;
	for (slot = 0; slot < TIMING_SLOTS; slot++)
		if (dev->vk.CreateSemaphore(dev->handle, &binary, NULL,
				&dev->timing.hop[slot]) != VK_SUCCESS
				|| !record_begin(dev, slot) || !record_end(dev, slot))
			return 0;
	return 1;
}

void
timing_destroy(struct device *dev)
{
	struct timing *t = &dev->timing;
	uint32_t slot;

	for (slot = 0; slot < TIMING_SLOTS; slot++)
		dev->vk.DestroySemaphore(dev->handle, t->hop[slot], NULL);
	dev->vk.DestroyBuffer(dev->handle, t->scratch, NULL);
	dev->vk.FreeMemory(dev->handle, t->scratch_memory, NULL);
}

void
timing_claim(struct device *dev, struct extras *x)
{
	struct timing *t = &dev->timing;
	uint32_t slot;

	if (t->head - t->tail >= TIMING_SLOTS)
		return;
	slot = t->head++ % TIMING_SLOTS;
	t->frame[slot] = dev->frames;
	t->level[slot] = atomic_load(&dev->level);
	x->begin = t->begin[slot];
	x->end = t->end[slot];
	x->hop = t->hop[slot];
}

void
timing_abandon(struct device *dev, const struct extras *x)
{
	if (x->begin)
		dev->timing.head--;
}

/* Sampled once next frame starts. */
static void
account(struct device *dev, uint32_t slot, struct sample part)
{
	struct timing *t = &dev->timing;
	struct sample whole = { t->sum_level, t->sum_ns, part.at };

	if (t->frame[slot] == t->sum_frame) {
		t->sum_ns += part.gpu_ns;
		return;
	}
	if (whole.gpu_ns > 0.0f && whole.gpu_ns < MAX_FRAME_NS)
		control_sample(&dev->control, &whole);
	t->sum_frame = t->frame[slot];
	t->sum_level = t->level[slot];
	t->sum_ns = part.gpu_ns;
}

/* Stale slots read as pending. */
void
timing_harvest(struct device *dev, uint64_t now)
{
	struct timing *t = &dev->timing;
	struct stamp q[STAMPS];
	uint32_t slot;

	while (t->tail != t->head) {
		slot = t->tail % TIMING_SLOTS;
		q[0].available = q[1].available = 0;
		dev->vk.GetQueryPoolResults(dev->handle, dev->queries, START_QUERY(slot),
			STAMPS, sizeof(q), q, sizeof(q[0]),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (!q[0].available || !q[1].available
				|| ((q[0].value - t->last_end) & t->mask) > t->mask / 2)
			return;
		account(dev, slot, (struct sample){
			.gpu_ns = (float)((q[1].value - q[0].value) & t->mask) * t->period,
			.at = now,
		});
		t->last_end = q[1].value;
		t->tail++;
	}
}

uint32_t
timing_mark(const struct device *dev)
{
	return dev->timing.head;
}

/* Mark's work has finished. */
int
timing_passed(const struct device *dev, uint32_t mark)
{
	return (int32_t)(dev->timing.tail - mark) > 0;
}
