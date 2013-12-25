/*
	Copyright (C) 2013 University of Lugano

	This file is part of LibPaxos.

	LibPaxos is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Libpaxos is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LibPaxos.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "evpaxos.h"
#include "peers.h"
#include "config_reader.h"
#include "libpaxos_messages.h"
#include "tcp_sendbuf.h"
#include "tcp_receiver.h"
#include "proposer.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

struct evproposer
{
	int id;
	int preexec_window;
	struct tcp_receiver* receiver;      //接收器
	struct event_base* base;
	struct proposer* state;      //proposer状态机
	struct peers* acceptors;            //接受点集合
	struct timeval tv;
	struct event* timeout_ev;
};


static void
send_prepares(struct evproposer* p, prepare_req* pr)
{
	int i;
	for (i = 0; i < peers_count(p->acceptors); i++) {
		struct bufferevent* bev = peers_get_buffer(p->acceptors, i);     //获得特定acceptor的bufferevent，并将prepare请求放入相应的buffer中

		sendbuf_add_prepare_req(bev, pr);
	}
}

static void
send_accepts(struct evproposer* p, accept_req* ar)
{
	int i;
	for (i = 0; i < peers_count(p->acceptors); i++) {
		struct bufferevent* bev = peers_get_buffer(p->acceptors, i);
    	sendbuf_add_accept_req(bev, ar);

	}
}

static void
proposer_preexecute(struct evproposer* p)
{
	int i;
	prepare_req pr;
	int count = p->preexec_window - proposer_prepared_count(p->state);  //先进先出队列保存准备实例，队列长128，用一个释放一个，有空间了就把新的加到队列末尾。
	if (count <= 0) return;
	for (i = 0; i < count; i++) {
		proposer_prepare(p->state, &pr);              //发送prepare请求
		send_prepares(p, &pr);
	}
	paxos_log_debug("Opened %d new instances", count);
	
}

static void
try_accept(struct evproposer* p)                                        //proposer将value等包装成accept_req包，交给evacceptor，后者将他们放入bufferevent中发送
{
	accept_req* ar;
	while ((ar = proposer_accept(p->state)) != NULL) {   //检验是否有propose实例，如果没有，执行execute
		send_accepts(p, ar);
		free(ar);
	}
	proposer_preexecute(p);
}

static void
proposer_handle_prepare_ack(struct evproposer* p, prepare_ack* ack)
{
	prepare_req pr;
	int preempted = proposer_receive_prepare_ack(p->state, ack, &pr);
	if (preempted)
		send_prepares(p, &pr);
}

static void
proposer_handle_accept_ack(struct evproposer* p, accept_ack* ack)
{
	prepare_req pr;
	int preempted = proposer_receive_accept_ack(p->state, ack, &pr);
	if (preempted)
		send_prepares(p, &pr);
}

static void
proposer_handle_client_msg(struct evproposer* p, char* value, int size)
{
	proposer_propose(p->state, value, size);
}

static void
proposer_handle_msg(struct evproposer* p, struct bufferevent* bev)       //proposer处理各种类型的消息
{
	paxos_msg msg;
	struct evbuffer* in;
	char buffer[PAXOS_MAX_VALUE_SIZE];

	in = bufferevent_get_input(bev);
	evbuffer_remove(in, &msg, sizeof(paxos_msg));            //将消息文件头从输入buffer移动到msg中
	if (msg.data_size > PAXOS_MAX_VALUE_SIZE) {           //消息内容过大，丢弃
		evbuffer_drain(in, msg.data_size);
		paxos_log_error("Discarding message of size %ld. Maximum is %d",
			msg.data_size, PAXOS_MAX_VALUE_SIZE);
		return;
	}
	evbuffer_remove(in, buffer, msg.data_size);         //将消息的内容移动到buffer中
	
	switch (msg.type) {
		case prepare_acks:
			proposer_handle_prepare_ack(p, (prepare_ack*)buffer);
			break;
		case accept_acks:
			proposer_handle_accept_ack(p, (accept_ack*)buffer);
			break;
		case submit:
			proposer_handle_client_msg(p, buffer, msg.data_size);
			break;
		default:
			paxos_log_error("Unknow msg type %d not handled", msg.type);
			return;
	}
	
	try_accept(p);
}

static void
handle_request(struct bufferevent* bev, void* arg)
{
	size_t len;
	paxos_msg msg;
	struct evproposer* p = arg;
	struct evbuffer* in = bufferevent_get_input(bev);
	
	while ((len = evbuffer_get_length(in)) > sizeof(paxos_msg)) {         //如果buffer中的数据数量大于一定，开始处理
		evbuffer_copyout(in, &msg, sizeof(paxos_msg));             //判断一下，如果大小还小于消息大小，说明未传输完成，返回，否则处理消息
		if (len < PAXOS_MSG_SIZE((&msg)))
			return;
		proposer_handle_msg(p, bev);
	}
}

static void
proposer_check_timeouts(evutil_socket_t fd, short event, void *arg)
{
	struct evproposer* p = arg;
	struct timeout_iterator* iter = proposer_timeout_iterator(p->state);
	
	prepare_req* pr;
	while ((pr = timeout_iterator_prepare(iter)) != NULL) {
		paxos_log_info("Instance %d timed out.", pr->iid);
		send_prepares(p, pr);
		free(pr);
	}
	
	accept_req* ar;
	while ((ar = timeout_iterator_accept(iter)) != NULL) {
		paxos_log_info("Instance %d timed out.", ar->iid);
		send_accepts(p, ar);
		free(ar);
	}
	
	timeout_iterator_free(iter);
	event_add(p->timeout_ev, &p->tv);   //Thus, if you want to make the event pending again, you can call event_add() on it again from inside the callback function.将超时事件重新加入
}

struct evproposer*
evproposer_init(int id, const char* config_file, struct event_base* b)
{
	int i;
	struct evproposer* p;
	
	struct config* conf = read_config(config_file);        //读取配置文件
	if (conf == NULL)
		return NULL;
	
	// Check id validity of proposer_id
	if (id < 0 || id >= MAX_N_OF_PROPOSERS) {                //检查proposerid
		paxos_log_error("Invalid proposer id: %d", id);
		return NULL;
	}

	p = malloc(sizeof(struct evproposer));

	p->id = id;
	p->base = b;
	p->preexec_window = paxos_config.proposer_preexec_window;    //128
		
	// Setup client listener
	p->receiver = tcp_receiver_new(b, &conf->proposers[id], handle_request, p);    //创建新的接收器
	
	// Setup connections to acceptors
	p->acceptors = peers_new(b, conf->acceptors_count);      //连接池
	for (i = 0; i < conf->acceptors_count; i++)
		peers_connect(p->acceptors, &conf->acceptors[i], handle_request, p);   //连接各个acceptor
	
	// Setup timeout
	p->tv.tv_sec = paxos_config.proposer_timeout;
	p->tv.tv_usec = 0;
	p->timeout_ev = evtimer_new(b, proposer_check_timeouts, p);
	event_add(p->timeout_ev, &p->tv);                         //添加超时事件
	
	p->state = proposer_new(p->id, conf->acceptors_count);  //创建新的proposer状态机
	
	free_config(conf);
	return p;
}

void
evproposer_free(struct evproposer* p)
{
	peers_free(p->acceptors);
	tcp_receiver_free(p->receiver);
	proposer_free(p->state);
	free(p);
}
