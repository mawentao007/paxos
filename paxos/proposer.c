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


#include "proposer.h"
#include "carray.h"
#include "quorum.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

struct instance
{
	iid_t iid;
	ballot_t ballot;
	ballot_t value_ballot;
	paxos_msg* value;
	int closed;
	struct quorum quorum;
	struct timeval created_at;
};

struct proposer 
{
	int id;
	int acceptors;
	struct carray* values;         //要接受的值的队列
	iid_t next_prepare_iid;
	struct carray* prepare_instances; /* Instances waiting for prepare acks 接收prepare回复的实例 */
	// TODO accept_instances should be a hash table
	struct carray* accept_instances; /* Instance waiting for accept acks 接受accept回复的实例*/
};

struct timeout_iterator
{
	int pi, ai;
	struct timeval timeout;
	struct proposer* proposer;
};

static struct instance* instance_new(iid_t iid, ballot_t ballot, int acceptors);
static void instance_free(struct instance* inst);
static struct instance* instance_find(struct carray* c, iid_t iid);
static int instance_match(void* arg, void* item);
static struct carray* instance_remove(struct carray* c, struct instance* inst);
static int instance_has_timedout(struct instance* inst, struct timeval* now);
static paxos_msg* wrap_value(char* value, size_t size);
static void prepare_preempt(struct proposer* p, struct instance* inst, prepare_req* out);
static ballot_t proposer_next_ballot(struct proposer* p, ballot_t b);
static int timeval_diff(struct timeval* t1, struct timeval* t2);


struct proposer*
proposer_new(int id, int acceptors)       //创建新的proposer
{
	struct proposer *p;
	int instances = 128;
	p = malloc(sizeof(struct proposer));
	p->id = id;
	p->acceptors = acceptors;         //acceptor个数
	p->values = carray_new(instances);//instance的实例队列
	p->next_prepare_iid = 0;
	p->prepare_instances = carray_new(instances);
	p->accept_instances = carray_new(instances);
	return p;
}

void
proposer_free(struct proposer* p)                  //释放proposer
{
	int i;
	for (i = 0; i < carray_count(p->values); ++i)     //挨个释放实例空间
		free(carray_at(p->values, i));
	carray_free(p->values);
	for (i = 0; i < carray_count(p->prepare_instances); ++i)//释放prepare和accept请求的实例队列
		instance_free(carray_at(p->prepare_instances, i));
	carray_free(p->prepare_instances);
	for (i = 0; i < carray_count(p->accept_instances); ++i)
		instance_free(carray_at(p->accept_instances, i));
	carray_free(p->accept_instances);
	free(p);
}

struct timeout_iterator*
proposer_timeout_iterator(struct proposer* p)                   //超时迭代器
{
	struct timeout_iterator* iter;
	iter = malloc(sizeof(struct timeout_iterator));
	iter->pi = iter->ai = 0;
	iter->proposer = p;
	gettimeofday(&iter->timeout, NULL);
	return iter;
}

void
proposer_propose(struct proposer* p, char* value, size_t size)   //提出propose,将msg中的value放入proposer的value队列中
{
	paxos_msg* msg;
	msg = wrap_value(value, size);
	carray_push_back(p->values, msg);
}

int
proposer_prepared_count(struct proposer* p)            //prepare实例的数量
{
	return carray_count(p->prepare_instances);
}

void
proposer_prepare(struct proposer* p, prepare_req* out)       //prepare请求
{
	struct instance* inst;
	iid_t iid = ++(p->next_prepare_iid);
	inst = instance_new(iid, proposer_next_ballot(p, 0), p->acceptors); //包装一个instance实例放到prepare实例队列末尾
	carray_push_back(p->prepare_instances, inst);
	*out = (prepare_req) {inst->iid, inst->ballot};
}

int
proposer_receive_prepare_ack(struct proposer* p, prepare_ack* ack,      //接受准备请求的回复
	prepare_req* out)
{
	struct instance* inst;
	
	inst = instance_find(p->prepare_instances, ack->iid);         //查找相应实例
	
	if (inst == NULL) {                                            //实例为空，表示相应实例没有发生
		paxos_log_debug("Promise dropped, instance %u not pending", ack->iid);
		return 0;
	}
	
	if (ack->ballot < inst->ballot) {                                       //如果实例的最高编号大于当前的编号，当前的太老了，被丢弃
		paxos_log_debug("Promise dropped, too old");
		return 0;
	}
	
	if (ack->ballot == inst->ballot) {	// preempted?                          //
		
		if (!quorum_add(&inst->quorum, ack->acceptor_id)) {           //如果之前相应的acceptor已经被加入到多数派中
			paxos_log_debug("Promise dropped from %d, instance %u has a quorum",
				ack->acceptor_id, inst->iid);
			return 0;
		}
		
		paxos_log_debug("Received valid promise from: %d, iid: %u",
			ack->acceptor_id, inst->iid);
		
		if (ack->value_size > 0) {                                   //如果acceptor已经接受其它值
			paxos_log_debug("Promise has value");
			if (inst->value == NULL) {                      //当前实例未赋值，将回复中的值赋给它
				inst->value_ballot = ack->value_ballot;
				inst->value = wrap_value(ack->value, ack->value_size);
			} else if (ack->value_ballot > inst->value_ballot) {     //当前实例赋值，但是消息的投票比自己的要新，那么就使用新的
				carray_push_back(p->values, inst->value);    //有初值的话，将当前的value放到value队列末尾，并更新实例的value。
				inst->value_ballot = ack->value_ballot;
				inst->value = wrap_value(ack->value, ack->value_size);
				paxos_log_debug("Value in promise saved, removed older value");
			} 
			/*else if (ack->value_ballot == inst->value_ballot) {        //该值已经被接受，关闭实例
				// TODO this assumes that the QUORUM is 2!
				paxos_log_debug("Instance %d closed", inst->iid);
				inst->closed = 1;
			}*/// ?????????????? 
			else {
				paxos_log_debug("Value in promise ignored");
			}
		}
		
		return 0;
		
	} else {
		paxos_log_debug("Instance %u preempted: ballot %d ack ballot %d",
			inst->iid, inst->ballot, ack->ballot);
		prepare_preempt(p, inst, out);        //preempt 取代
		return 1;
	}
}

accept_req* 
proposer_accept(struct proposer* p)   //发起accept请求                      //将buffer传入的值放到proposer的value队列中，在发送accept请求的时候取出来用。
{
	struct instance* inst;

	// is there a prepared instance?   检查是否已经有过prepare实例，如果实例关闭，释放prepare实例，如果没达到多数派,等待当前实例完成
	while ((inst = carray_front(p->prepare_instances)) != NULL) {
		if (inst->closed)                                                      //如果实例已经关闭，释放掉所有相应实例的所有prepare实例
			instance_free(carray_pop_front(p->prepare_instances));
		else if (!quorum_reached(&inst->quorum))               //没达到多数派的时候，返回空,说明该实例的准备工作未完成
			return NULL;
		else break;
	}
	
	if (inst == NULL)
		return NULL;
	
	paxos_log_debug("Trying to accept iid %u", inst->iid);
	
	// is there a value?
	if (inst->value == NULL) {       //如果实例没有值，查看值队列中是否有
		inst->value = carray_pop_front(p->values);
		if (inst->value == NULL) {
			paxos_log_debug("No value to accept");
			return NULL;	
		}
		paxos_log_debug("Popped next value");
	} else {
		paxos_log_debug("Instance has value");
	}
	
	// we have both a prepared instance and a value
	inst = carray_pop_front(p->prepare_instances);    //有值，有prepare请求
	quorum_clear(&inst->quorum);           //清除实例中的多数派计数器
	carray_push_back(p->accept_instances, inst);         //将实例加入到队列accept末尾
	
	accept_req* req = malloc(sizeof(accept_req) + inst->value->data_size); //创建接收请求
	req->iid = inst->iid;                 //接受队列实例号就是当前实例号
	req->ballot = inst->ballot;           
	req->value_size = inst->value->data_size;
	memcpy(req->value, inst->value->data, req->value_size);

	return req;
}

int
proposer_receive_accept_ack(struct proposer* p, accept_ack* ack, prepare_req* out)       //接受acceptor对于accept请求的回复
{
	struct instance* inst;
	
	inst = instance_find(p->accept_instances, ack->iid);                 //找到相应实例
	
	if (inst == NULL) {
		paxos_log_debug("Accept ack dropped, iid: %u not pending", ack->iid);        //实例不存在了
		return 0;
	}
	
	if (ack->ballot == inst->ballot) {                                    //已经接收过一次了
		assert(ack->value_ballot == inst->ballot);
		if (!quorum_add(&inst->quorum, ack->acceptor_id)) { 
			paxos_log_debug("Dropping duplicate accept from: %d, iid: %u", 
				ack->acceptor_id, inst->iid);
			return 0;
		}
											
		if (quorum_reached(&inst->quorum)) {      //达成多数派
			paxos_log_debug("Quorum reached for instance %u", inst->iid);
			p->accept_instances = instance_remove(p->accept_instances, inst);  //返回除去inst以外的实例队列
			instance_free(inst);        //将完成的实例inst释放掉
		}
		
		return 0;
		
	} else {                                                           //收到的回复不是当前proposer的提案号，移除出队列，重新prepare
		paxos_log_debug("Instance %u preempted: ballot %d ack ballot %d",
			inst->iid, inst->ballot, ack->ballot);
		
		p->accept_instances = instance_remove(p->accept_instances, inst);
		carray_push_front(p->prepare_instances, inst);
		prepare_preempt(p, inst, out);
		return  1; 
	}
}

static struct instance*
next_timedout(struct carray* c, int* i, struct timeval* t)        //返回超时实例
{
	struct instance* inst;
	while (*i < carray_count(c)) {
		inst = carray_at(c, *i);
		(*i)++;
		if (quorum_reached(&inst->quorum))   //达到多数派，继续；超时，返回
			continue;
		if (instance_has_timedout(inst, t))
			return inst;
	}
	return NULL;
}

prepare_req*
timeout_iterator_prepare(struct timeout_iterator* iter)     //超时请求，迭代准备
{
	struct instance* inst;
	struct proposer* p = iter->proposer;
	inst = next_timedout(p->prepare_instances, &iter->pi, &iter->timeout);  //找到超时实例,重新打包
	if (inst != NULL) {
		prepare_req* req = malloc(sizeof(prepare_req));
		*req = (prepare_req){inst->iid, inst->ballot};
		inst->created_at = iter->timeout;
		return req;
	}
	return NULL;
}

accept_req*
timeout_iterator_accept(struct timeout_iterator* iter)       //迭代发送accept包
{
	struct instance* inst;
	struct proposer* p = iter->proposer;
	inst = next_timedout(p->accept_instances, &iter->ai, &iter->timeout);   //找到超时的accept实例，重新打包
	if (inst != NULL) {
		accept_req* req = malloc(sizeof(accept_req) + inst->value->data_size);
		req->iid = inst->iid;
		req->ballot = inst->ballot;
		req->value_size = inst->value->data_size;
		memcpy(req->value, inst->value->data, req->value_size);
		inst->created_at = iter->timeout;
		return req;
	}
	return NULL;
}

void
timeout_iterator_free(struct timeout_iterator* iter)            //释放迭代器
{
	free(iter);
}

static struct instance*
instance_new(iid_t iid, ballot_t ballot, int acceptors)       //创建新实例
{
	struct instance* inst;
	inst = malloc(sizeof(struct instance));
	inst->iid = iid;
	inst->ballot = ballot;
	inst->value_ballot = 0;
	inst->value = NULL;
	inst->closed = 0;
	gettimeofday(&inst->created_at, NULL);
	quorum_init(&inst->quorum, acceptors);
	return inst;
}

static void
instance_free(struct instance* inst)              //释放实例
{ 
	quorum_destroy(&inst->quorum);     //销毁多数派，释放值，是否实例
	if (inst->value != NULL)
		free(inst->value);
	free(inst);
}

static int
instance_has_timedout(struct instance* inst, struct timeval* now)   //检查实例是否超时
{
	int diff = timeval_diff(&inst->created_at, now);
	return diff >= paxos_config.proposer_timeout;
}

static struct instance*
instance_find(struct carray* c, iid_t iid)         //查找相应实例 
{
	int i;
	for (i = 0; i < carray_count(c); ++i) {
		struct instance* inst = carray_at(c, i);
		if (inst->iid == iid)
			return carray_at(c, i);
	}
	return NULL;
}

static int
instance_match(void* arg, void* item)        //两个实例匹配
{
	struct instance* a = arg;
	struct instance* b = item;
    return a->iid == b->iid;
}

static struct carray*
instance_remove(struct carray* c, struct instance* inst)   //返回除去inst以外的队列
{
	struct carray* tmp;
	tmp = carray_reject(c, instance_match, inst);
	carray_free(c);
	return tmp;
}

static paxos_msg*
wrap_value(char* value, size_t size)    //将value包装成paxos_msg
{
	paxos_msg* msg = malloc(size + sizeof(paxos_msg));
	msg->data_size = size;
	msg->type = submit;
	memcpy(msg->data, value, size);
	return msg;
}

void
prepare_preempt(struct proposer* p, struct instance* inst, prepare_req* out)     //更改提案号到一个更大的，重写发送req请求
{
	inst->ballot = proposer_next_ballot(p, inst->ballot);
	quorum_clear(&inst->quorum);
	*out = (prepare_req) {inst->iid, inst->ballot};
	gettimeofday(&inst->created_at, NULL);
}

static ballot_t
proposer_next_ballot(struct proposer* p, ballot_t b)    //下一个提案号
{
	if (b > 0)
		return MAX_N_OF_PROPOSERS + b;
	else
		return MAX_N_OF_PROPOSERS + p->id;
}

/* Returns t2 - t1 in microseconds. */
static int
timeval_diff(struct timeval* t1, struct timeval* t2)   //返回时间差
{
    int us;
    us = (t2->tv_sec - t1->tv_sec) * 1e6;
    if (us < 0) return 0;
    us += (t2->tv_usec - t1->tv_usec);
    return us;
}
