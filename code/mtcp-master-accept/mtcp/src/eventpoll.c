#include <sys/queue.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <assert.h>

#include "mtcp.h"
#include "tcp_stream.h"
#include "eventpoll.h"
#include "tcp_in.h"
#include "pipe.h"
#include "debug.h"

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define SPIN_BEFORE_SLEEP FALSE
#define SPIN_THRESH 10000000

/*----------------------------------------------------------------------------*/
char *event_str[] = {"NONE", "IN", "PRI", "OUT", "ERR", "HUP", "RDHUP"};
/*----------------------------------------------------------------------------*/
char * 
EventToString(uint32_t event)
{
	switch (event) {
		case MTCP_EPOLLNONE:
			return event_str[0];
			break;
		case MTCP_EPOLLIN:
			return event_str[1];
			break;
		case MTCP_EPOLLPRI:
			return event_str[2];
			break;
		case MTCP_EPOLLOUT:
			return event_str[3];
			break;
		case MTCP_EPOLLERR:
			return event_str[4];
			break;
		case MTCP_EPOLLHUP:
			return event_str[5];
			break;
		case MTCP_EPOLLRDHUP:
			return event_str[6];
			break;
		default:
			assert(0);
	}
	
	assert(0);
	return NULL;
}
/*----------------------------------------------------------------------------*/
struct event_queue *
CreateEventQueue(int size)
{
	struct event_queue *eq;

	eq = (struct event_queue *)calloc(1, sizeof(struct event_queue));
	if (!eq)
		return NULL;

	eq->start = 0;
	eq->end = 0;
	eq->size = size;
	eq->events = (struct mtcp_epoll_event_int *)
			calloc(size, sizeof(struct mtcp_epoll_event_int));
	if (!eq->events) {
		free(eq);
		return NULL;
	}
	eq->num_events = 0;

	return eq;
}
/*----------------------------------------------------------------------------*/
void 
DestroyEventQueue(struct event_queue *eq)
{
	if (eq->events)
		free(eq->events);

	free(eq);
}
/*----------------------------------------------------------------------------*/
void
mtcp_epoll_worker_create(mctx_t mctx, int size)
{
	mtcp_manager_t mtcp = g_mtcp[mctx->cpu];
	mtcp_manager_t mtcp_master = g_mtcp[0];
	struct mtcp_epoll *ep;
	socket_map_t epsocket;

	if (size <= 0) {
		errno = EINVAL;
		return ;
	}
	
	epsocket = AllocateSocket(mctx, MTCP_SOCK_EPOLL, FALSE);
	if (!epsocket) {
		errno = ENFILE;
		return ;
	}

	ep = (struct mtcp_epoll *)calloc(1, sizeof(struct mtcp_epoll));
	if (!ep) {
		FreeSocket(mctx, epsocket->id, FALSE);
		return ;
	}

	/* create event queues */
	ep->usr_queue = CreateEventQueue(size);
	if (!ep->usr_queue) {
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return ;
	}

	ep->usr_shadow_queue = CreateEventQueue(size);
	if (!ep->usr_shadow_queue) {
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return ;
	}

	ep->mtcp_queue = CreateEventQueue(size);
	if (!ep->mtcp_queue) {
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return ;
	}

	mtcp->ep = ep;
	mtcp->ep_master = mtcp_master->ep;
	epsocket->ep = ep;

	if (pthread_mutex_init(&ep->epoll_lock, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return ;
	}
	if (pthread_cond_init(&ep->epoll_cond, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return ;
	}
	
	if(ep!=NULL) printf("[eventpoll.c mtcp_epoll_worker] epoll worker created %d\n", mtcp->ctx->cpu);
}

int
mtcp_epoll_create(mctx_t mctx, int size)
{
	mtcp_manager_t mtcp = g_mtcp[mctx->cpu];
	struct mtcp_epoll *ep;
	socket_map_t epsocket;

	if (size <= 0) {
		errno = EINVAL;
		return -1;
	}

	epsocket = AllocateSocket(mctx, MTCP_SOCK_EPOLL, FALSE);
	if (!epsocket) {
		errno = ENFILE;
		return -1;
	}
	
	
	ep = (struct mtcp_epoll *)calloc(1, sizeof(struct mtcp_epoll));
	if (!ep) {
		FreeSocket(mctx, epsocket->id, FALSE);
		return -1;
	}

	/* create event queues */
	ep->usr_queue = CreateEventQueue(size);
	if (!ep->usr_queue) {
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	ep->usr_shadow_queue = CreateEventQueue(size);
	if (!ep->usr_shadow_queue) {
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	ep->mtcp_queue = CreateEventQueue(size);
	if (!ep->mtcp_queue) {
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	ep->mtcp_accept = CreateEventQueue(size);
	if (!ep->mtcp_accept) {
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	TRACE_EPOLL("epoll structure of size %d created.\n", size);

	mtcp->ep = ep;
	mtcp->ep_master = NULL;
	if(mtcp->w_dsmap != NULL) mtcp->ep_master = g_mtcp[0]->ep;
	epsocket->ep = ep;

	if (pthread_mutex_init(&ep->epoll_lock, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}
	if (pthread_cond_init(&ep->epoll_cond, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	if(mtcp->w_dsmap != NULL) mtcp->smap[epsocket->id] = *epsocket;
	return epsocket->id;
}
/*----------------------------------------------------------------------------*/
int 
CloseEpollSocket(mctx_t mctx, int epid)
{
	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;
	PROMOD_BEGIN
	
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		return -1;
	}

	
	ep = mtcp->smap[epid].ep;
	if (!ep) {
		errno = EINVAL;
		return -1;
	}

	DestroyEventQueue(ep->usr_queue);
	DestroyEventQueue(ep->usr_shadow_queue);
	DestroyEventQueue(ep->mtcp_queue);
	DestroyEventQueue(ep->mtcp_accept);

	PROMOD_TS0
	pthread_mutex_lock(&ep->epoll_lock);
	PROMOD_TS1(PRO_EPOLL, "EPOLL")
	
	mtcp->ep = NULL;
	mtcp->smap[epid].ep = NULL;
	pthread_cond_signal(&ep->epoll_cond);
	pthread_mutex_unlock(&ep->epoll_lock);

	pthread_cond_destroy(&ep->epoll_cond);
	pthread_mutex_destroy(&ep->epoll_lock);
	free(ep);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int 
RaisePendingStreamEvents(mtcp_manager_t mtcp, 
		struct mtcp_epoll *ep, socket_map_t socket)
{
	tcp_stream *stream = socket->stream;

	if (!stream)
		return -1;
	if (stream->state < TCP_ST_ESTABLISHED)
		return -1;

	TRACE_EPOLL("Stream %d at state %s\n", 
			stream->id, TCPStateToString(stream));
	/* if there are payloads already read before epoll registration */
	/* generate read event */
	if (socket->epoll & MTCP_EPOLLIN) {
		struct tcp_recv_vars *rcvvar = stream->rcvvar;
		if (rcvvar->rcvbuf && rcvvar->rcvbuf->merged_len > 0) {
			TRACE_EPOLL("Socket %d: Has existing payloads\n", socket->id);
			AddEpollEvent(mtcp, ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLIN);
		} else if (stream->state == TCP_ST_CLOSE_WAIT) {
			TRACE_EPOLL("Socket %d: Waiting for close\n", socket->id);
			AddEpollEvent(mtcp, ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLIN);
		}
	}

	/* same thing to the write event */
	if (socket->epoll & MTCP_EPOLLOUT) {
		struct tcp_send_vars *sndvar = stream->sndvar;
		if (!sndvar->sndbuf || 
				(sndvar->sndbuf && sndvar->sndbuf->len < sndvar->snd_wnd)) {
			if (!(socket->events & MTCP_EPOLLOUT)) {
				TRACE_EPOLL("Socket %d: Adding write event\n", socket->id);
				AddEpollEvent(mtcp, ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLOUT);
			}
		}
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
mtcp_epoll_ctl(mctx_t mctx, int epid, 
		int op, int sockid, struct mtcp_epoll_event *event)
{
	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;
	socket_map_t socket;
	uint32_t events;

	mtcp = g_mtcp[mctx->cpu];
	if (!mtcp) {
		return -1;
	}

	if (epid < 0 || epid >= CONFIG.max_concurrency) {
		TRACE_API("Epoll id %d out of range.\n", epid);
		errno = EBADF;
		return -1;
	}

	if (sockid < 0 || sockid >= CONFIG.max_concurrency) {
		TRACE_API("Socket id %d out of range.\n", sockid);
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype == MTCP_SOCK_UNUSED) {
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype != MTCP_SOCK_EPOLL) {
		errno = EINVAL;
		return -1;
	}

	ep = mtcp->smap[epid].ep;
	if (!ep || (!event && op != MTCP_EPOLL_CTL_DEL)) {
		errno = EINVAL;
		return -1;
	}
	socket = &mtcp->smap[sockid];

	if (op == MTCP_EPOLL_CTL_ADD) {
		if (socket->epoll) {
			errno = EEXIST;
			return -1;
		}

		/* EPOLLERR and EPOLLHUP are registered as default */
		events = event->events;
		events |= (MTCP_EPOLLERR | MTCP_EPOLLHUP);
		socket->ep_data = event->data;
		socket->epoll = events;

		TRACE_EPOLL("Adding epoll socket %d(type %d) ET: %u, IN: %u, OUT: %u\n", 
				socket->id, socket->socktype, socket->epoll & MTCP_EPOLLET, 
				socket->epoll & MTCP_EPOLLIN, socket->epoll & MTCP_EPOLLOUT);

		if (socket->socktype == MTCP_SOCK_STREAM) {
			RaisePendingStreamEvents(mtcp, ep, socket);
		} else if (socket->socktype == MTCP_SOCK_PIPE) {
			RaisePendingPipeEvents(mctx, epid, sockid);
		}

	} else if (op == MTCP_EPOLL_CTL_MOD) {
		if (!socket->epoll) {
			pthread_mutex_unlock(&ep->epoll_lock);
			errno = ENOENT;
			return -1;
		}

		events = event->events;
		events |= (MTCP_EPOLLERR | MTCP_EPOLLHUP);
		socket->ep_data = event->data;
		socket->epoll = events;

		if (socket->socktype == MTCP_SOCK_STREAM) {
			RaisePendingStreamEvents(mtcp, ep, socket);
		} else if (socket->socktype == MTCP_SOCK_PIPE) {
			RaisePendingPipeEvents(mctx, epid, sockid);
		}

	} else if (op == MTCP_EPOLL_CTL_DEL) {
		if (!socket->epoll) {
			errno = ENOENT;
			return -1;
		}

		socket->epoll = MTCP_EPOLLNONE;
	}

	//g_wcpu[socket->id]=0;

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
mtcp_epoll_wait(mctx_t mctx, int epid, 
		struct mtcp_epoll_event *events, int maxevents, int timeout)
{
	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;
	struct event_queue *eq;
	struct event_queue *eq_shadow;
	socket_map_t event_socket;
	int validity;
	int i, cnt, ret;
	int num_events;
	PROMOD_BEGIN
	
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		return -1;
	}

	if (epid < 0 || epid >= CONFIG.max_concurrency) {
		TRACE_API("Epoll id %d out of range.\n", epid);
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype == MTCP_SOCK_UNUSED) {
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype != MTCP_SOCK_EPOLL) {
		errno = EINVAL;
		return -1;
	}

	ep = mtcp->smap[epid].ep;
	if (!ep || !events || maxevents <= 0) {
		errno = EINVAL;
		return -1;
	}

	ep->stat.calls++;

	PROMOD_TS0
#if SPIN_BEFORE_SLEEP
	int spin = 0;
	while (ep->num_events == 0 && spin < SPIN_THRESH) {
		spin++;
	}
#endif /* SPIN_BEFORE_SLEEP */

	if (pthread_mutex_lock(&ep->epoll_lock)) {
		PROMOD_TS1(PRO_EPOLL, "EPOLL")
		if (errno == EDEADLK)
			perror("mtcp_epoll_wait: epoll_lock blocked\n");
		assert(0);
	}

wait:
	eq = ep->usr_queue;
	eq_shadow = ep->usr_shadow_queue;

	/* wait until event occurs */
	while (eq->num_events == 0 && eq_shadow->num_events == 0 && timeout != 0) {
#if INTR_SLEEPING_MTCP
		/* signal to mtcp thread if it is sleeping */
		if (mtcp->wakeup_flag && mtcp->is_sleeping) {
			pthread_kill(mtcp->ctx->thread, SIGUSR1);
		}
#endif
		ep->stat.waits++;
		ep->waiting = TRUE;
		if (timeout > 0) {
			struct timespec deadline;

			clock_gettime(CLOCK_REALTIME, &deadline);
			if (timeout >= 1000) {
				int sec;
				sec = timeout / 1000;
				deadline.tv_sec += sec;
				timeout -= sec * 1000;
			}

			deadline.tv_nsec += timeout * 1000000;

			if (deadline.tv_nsec >= 1000000000) {
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000;
			}

			//deadline.tv_sec = mtcp->cur_tv.tv_sec;
			//deadline.tv_nsec = (mtcp->cur_tv.tv_usec + timeout * 1000) * 1000;
			ret = pthread_cond_timedwait(&ep->epoll_cond, 
					&ep->epoll_lock, &deadline);
			if (ret && ret != ETIMEDOUT) {
				/* errno set by pthread_cond_timedwait() */
				pthread_mutex_unlock(&ep->epoll_lock);
				TRACE_ERROR("pthread_cond_timedwait failed. ret: %d, error: %s\n", 
						ret, strerror(errno));
				return -1;
			}
			timeout = 0;
		} else if (timeout < 0) {
			ret = pthread_cond_wait(&ep->epoll_cond, &ep->epoll_lock);
			if (ret) {
				/* errno set by pthread_cond_wait() */
				pthread_mutex_unlock(&ep->epoll_lock);
				TRACE_ERROR("pthread_cond_wait failed. ret: %d, error: %s\n", 
						ret, strerror(errno));
				return -1;
			}
		}
		ep->waiting = FALSE;
		if (mtcp->ctx->done || mtcp->ctx->exit || mtcp->ctx->interrupt) {
			mtcp->ctx->interrupt = FALSE;
			//ret = pthread_cond_signal(&ep->epoll_cond);
			pthread_mutex_unlock(&ep->epoll_lock);
			errno = EINTR;
			return -1;
		}
		
	}
	
	/* fetch events from the user event queue */
	cnt = 0;
	num_events = eq->num_events;
	for (i = 0; i < num_events && cnt < maxevents; i++) {
		if(mtcp->w_dsmap == NULL)
			event_socket = &mtcp->smap[eq->events[eq->start].sockid];
		else event_socket = mtcp->w_dsmap->smap_ds[eq->events[eq->start].sockid];

		validity = TRUE;
		if (event_socket->socktype == MTCP_SOCK_UNUSED)
			validity = FALSE;
		if (!(event_socket->epoll & eq->events[eq->start].ev.events))
			validity = FALSE;
		if (!(event_socket->events & eq->events[eq->start].ev.events))
			validity = FALSE;

		if (validity) {
			events[cnt++] = eq->events[eq->start].ev;
			assert(eq->events[eq->start].sockid >= 0);

			TRACE_EPOLL("Socket %d: Handled event. event: %s, "
					"start: %u, end: %u, num: %u\n", 
					event_socket->id, 
					EventToString(eq->events[eq->start].ev.events), 
					eq->start, eq->end, eq->num_events);
			ep->stat.handled++;
		} else {
			TRACE_EPOLL("Socket %d: event %s invalidated.\n", 
					eq->events[eq->start].sockid, 
					EventToString(eq->events[eq->start].ev.events));
			ep->stat.invalidated++;
		}
		event_socket->events &= (~eq->events[eq->start].ev.events);

		eq->start++;
		eq->num_events--;
		if (eq->start >= eq->size) {
			eq->start = 0;
		}
	}

	/* fetch eventes from user shadow event queue */
	eq = ep->usr_shadow_queue;
	num_events = eq->num_events;
	for (i = 0; i < num_events && cnt < maxevents; i++) {
		if(mtcp->w_dsmap == NULL)
			event_socket = &mtcp->smap[eq->events[eq->start].sockid];
		else
			event_socket = mtcp->w_dsmap->smap_ds[eq->events[eq->start].sockid];	
		validity = TRUE;
		if (event_socket->socktype == MTCP_SOCK_UNUSED)
			validity = FALSE;
		if (!(event_socket->epoll & eq->events[eq->start].ev.events))
			validity = FALSE;
		if (!(event_socket->events & eq->events[eq->start].ev.events))
			validity = FALSE;

		if (validity) {
			events[cnt++] = eq->events[eq->start].ev;
			assert(eq->events[eq->start].sockid >= 0);

			TRACE_EPOLL("Socket %d: Handled event. event: %s, "
					"start: %u, end: %u, num: %u\n", 
					event_socket->id, 
					EventToString(eq->events[eq->start].ev.events), 
					eq->start, eq->end, eq->num_events);
			ep->stat.handled++;
		} else {
			TRACE_EPOLL("Socket %d: event %s invalidated.\n", 
					eq->events[eq->start].sockid, 
					EventToString(eq->events[eq->start].ev.events));
			ep->stat.invalidated++;
		}
		event_socket->events &= (~eq->events[eq->start].ev.events);

		eq->start++;
		eq->num_events--;
		if (eq->start >= eq->size) {
			eq->start = 0;
		}
	}

	if (cnt == 0 && timeout != 0)
		goto wait;

	pthread_mutex_unlock(&ep->epoll_lock);

	return cnt;
}
/*----------------------------------------------------------------------------*/
inline int 
AddEpollEvent(mtcp_manager_t mtcp, struct mtcp_epoll *ep, 
		int queue_type, socket_map_t socket, uint32_t event)
{
	struct event_queue *eq;
	int index;
	PROMOD_BEGIN

	if (!ep || !socket || !event){
		if(!ep) printf("No ep!\n");
		if(!socket) printf("No Sock!\n");
		if(!event) printf("No Ev!\n");
		return -1;
	}
	ep->stat.issued++;

	if (socket->events & event) {
		return 0;
	}

	if (queue_type == MTCP_EVENT_QUEUE) {
		eq = ep->mtcp_queue;
	} else if (queue_type == USR_EVENT_QUEUE) {
		eq = ep->usr_queue;
		PROMOD_TS0
		pthread_mutex_lock(&ep->epoll_lock);
		PROMOD_TS1(PRO_EPOLL, "EPOLL")
	} else if (queue_type == USR_SHADOW_EVENT_QUEUE) {
		eq = ep->usr_shadow_queue;
	} else if (queue_type == MTCP_EVENT_ACCEPT){
		eq = ep->mtcp_accept;
	} else {
		TRACE_ERROR("Non-existing event queue type!\n");
		return -1;
	}
	
	if (eq->num_events >= eq->size) {
		printf("Exceeded epoll event queue! num_events: %d, size: %d\n", 
				eq->num_events, eq->size);
		if (queue_type == USR_EVENT_QUEUE)
			pthread_mutex_unlock(&ep->epoll_lock);
		return -1;
	}

	index = eq->end++;

	socket->events |= event;
	eq->events[index].sockid = socket->id;
	eq->events[index].ev.events = event;
	eq->events[index].ev.data = socket->ep_data;

	if (eq->end >= eq->size) {
		eq->end = 0;
	}
	
	eq->num_events++;

#if 0
	TRACE_EPOLL("Socket %d New event: %s, start: %u, end: %u, num: %u\n",
			ep->events[index].sockid, 
			EventToString(ep->events[index].ev.events), 
			ep->start, ep->end, ep->num_events);
#endif

	if (queue_type == USR_EVENT_QUEUE)
		pthread_mutex_unlock(&ep->epoll_lock);

	ep->stat.registered++;

	return 0;
}
