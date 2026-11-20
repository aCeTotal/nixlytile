/* Jobs awaiting a slot. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#define QUEUE_MAX   8
#define CMD_LEN     16
#define ARG_LEN     48
#define PAYLOAD_MAX 2048

typedef struct {
	JobSpec spec;
	char cmd[CMD_LEN], arg[ARG_LEN];
	char payload[PAYLOAD_MAX];
	char ref[REF_LEN];
} Queued;

static Queued ring[QUEUE_MAX];
static int head, count;

/* Copies spec strings. */
int
queue_push(const JobSpec *s)
{
	Queued *q = &ring[(head + count) % QUEUE_MAX];

	if (count == QUEUE_MAX)
		return -1;
	snprintf(q->cmd, sizeof(q->cmd), "%s", s->cmd);
	snprintf(q->arg, sizeof(q->arg), "%s", s->arg ? s->arg : "");
	snprintf(q->payload, sizeof(q->payload), "%s",
			s->payload ? s->payload : "");
	snprintf(q->ref, sizeof(q->ref), "%s", s->ref ? s->ref : "");
	q->spec = (JobSpec){ .kind = s->kind, .cmd = q->cmd,
		.arg = s->arg ? q->arg : NULL, .payload = q->payload,
		.ref = q->ref, .token = s->token };
	count++;
	return 0;
}

const JobSpec *
queue_peek(void)
{
	return count ? &ring[head].spec : NULL;
}

void
queue_drop(void)
{
	explicit_bzero(&ring[head], sizeof(ring[head]));
	head = (head + 1) % QUEUE_MAX;
	count--;
}

int
queue_holds(JobKind kind)
{
	for (int i = 0; i < count; i++)
		if (ring[(head + i) % QUEUE_MAX].spec.kind == kind)
			return 1;
	return 0;
}
