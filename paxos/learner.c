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


#include "learner.h"
#include "carray.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct instance
{
	iid_t iid;
	ballot_t last_update_ballot;
	accept_ack** acks;
	accept_ack* final_value;
};

struct learner
{
	int quorum;
	int acceptors;
	int late_start;
	iid_t current_iid;
	iid_t highest_iid_seen;
	iid_t highest_iid_closed;
	struct carray* instances; 	/* TODO instances should be hashtable */ //实例队列
};


static void
instance_clear(struct instance* inst, int acceptors)  //清除实例
{
	int i;
	inst->iid = 0;                                 //将iid，上一次更新的投票值清零，将每个acceptors的回复释放，最终值置空
	inst->last_update_ballot = 0;
	for (i = 0; i < acceptors; i++) {
		if (inst->acks[i] != NULL) {
			free(inst->acks[i]);
			inst->acks[i] = NULL;
		}
	}
	inst->final_value = NULL;
}

static struct instance*
instance_new(int acceptors)                          //创建新的实例
{
	int i;
	struct instance* inst;
	inst = malloc(sizeof(struct instance));
	memset(inst, 0, sizeof(struct instance));  //初始化的时候都是0
	inst->acks = malloc(sizeof(accept_ack*) * acceptors);    //acceptors个数个回复，置空
	for (i = 0; i < acceptors; ++i)
		inst->acks[i] = NULL;
	return inst;
}

static void
instance_free(struct instance* inst, int acceptors)          //释放掉实例，先清空，后释放内存。
{
	instance_clear(inst, acceptors);
	free(inst->acks);
	free(inst);
}

/*
Checks if a given instance is closed, that is if a quorum of acceptor
accepted the same value ballot pair.
Returns 1 if the instance is closed, 0 otherwise
*/
static int 
instance_has_quorum(struct learner* l, struct instance* inst)      //检查一个实例是否关闭，也就是是否有多数派接受了同一个投票
{
	accept_ack * curr_ack;
	int i, a_valid_index = -1, count = 0;

	if (inst->final_value != NULL)               //有最终值，返回，已经关闭
		return 1;

	//Iterates over stored acks
	for (i = 0; i < l->acceptors; i++) {
		curr_ack = inst->acks[i];

		// skip over missing acceptor acks
		if (curr_ack == NULL)
			continue;

		// Count the ones "agreeing" with the last added
		if (curr_ack->ballot == inst->last_update_ballot) {
			count++;
			a_valid_index = i;

			// Special case: an acceptor is telling that
			// this value is -final-, it can be delivered immediately.      //特殊情况，一个acceptor声明这是最终值，直接返回
			if (curr_ack->is_final) {
				//For sure >= than quorum...
				count += l->acceptors;  //count值直接加上所有acceptor个数
				break;
			}
		}
	}
    
	//Reached a quorum/majority!
	if (count >= l->quorum) {               //是多数派，给最终值赋值
		paxos_log_debug("Reached quorum, iid: %u is closed!", inst->iid);
		inst->final_value = inst->acks[a_valid_index];
		return 1;
	}
    
	//No quorum yet...
	return 0;
}

/*
Adds the given accept_ack for the given instance.
Assumes inst->acks[acceptor_id] was already freed.
*/
static void
instance_add_accept(struct instance* inst, accept_ack* ack)  //添加新的accept_ack到实例中，索引为acceptor号，更新上次投票值  a-l
{
	accept_ack* new_ack;
	new_ack = malloc(ACCEPT_ACK_SIZE(ack));
	memcpy(new_ack, ack, ACCEPT_ACK_SIZE(ack));
	inst->acks[ack->acceptor_id] = new_ack;
	inst->last_update_ballot = ack->ballot;                        //只会接受更大的提案号，小的都被丢弃了
}

static struct instance*
learner_get_instance(struct learner* l, iid_t iid)       //
{
	struct instance* inst;
	inst = carray_at(l->instances, iid);       //取learner的instances中指定iid号的inst
	assert(inst->iid == iid || inst->iid == 0);
	return inst;
}

static struct instance*
learner_get_current_instance(struct learner* l)
{
	return learner_get_instance(l, l->current_iid);//取当前的instance
}

/*
	Tries to update the state based on the accept_ack received.
*/
static void
learner_update_instance(struct learner* l, accept_ack* ack)
{
	accept_ack* prev_ack;
	struct instance* inst = learner_get_instance(l, ack->iid);
	
	// First message for this iid            //相关instance的iid第一个消息,还未使用过该实例
	if (inst->iid == 0) {
		paxos_log_debug("Received first message for instance: %u", ack->iid);
		inst->iid = ack->iid;                 
		inst->last_update_ballot = ack->ballot;
	}
	assert(inst->iid == ack->iid);
	
	// Instance closed already, drop             //如果已经是多数派
	if (instance_has_quorum(l, inst)) {
		paxos_log_debug("Dropping accept_ack for iid %u, already closed",
			 ack->iid);
		return;
	}
	
	// No previous message to overwrite for this acceptor  //之前没对相应acceptor写过消息
	if (inst->acks[ack->acceptor_id] == NULL) {
		paxos_log_debug("Got first ack for: %u, acceptor: %d", 
			inst->iid, ack->acceptor_id);
		//Save this accept_ack
		instance_add_accept(inst, ack);    //添加并保存相应消息
		return;
	}
	
	// There is already a message from the same acceptor      已经接受过相同的消息
	prev_ack = inst->acks[ack->acceptor_id];
	
	// Already more recent info in the record, accept_ack is old        以前已经接受过更新的消息
	if (prev_ack->ballot >= ack->ballot) {
		paxos_log_debug("Dropping accept_ack for iid: %u,"
			"stored ballot is newer of equal ", ack->iid);
		return;
	}
	
	// Replace the previous ack since the received ballot is newer
	paxos_log_debug("Overwriting previous accept_ack for iid: %u", ack->iid);
	free(prev_ack);
	instance_add_accept(inst, ack);
}

accept_ack*
learner_deliver_next(struct learner* l)  //清除当前实例，返回应答消息
{
	struct instance* inst;
	accept_ack* ack = NULL;
	inst = learner_get_current_instance(l);
	if (instance_has_quorum(l, inst)) {
		size_t size = ACCEPT_ACK_SIZE(inst->final_value);
		ack = malloc(size);
		memcpy(ack, inst->final_value, size);
		instance_clear(inst, l->acceptors);
		l->current_iid++;
	}
	return ack;
}

void
learner_receive_accept(struct learner* l, accept_ack* ack)   //获得接收消息
{
	if (l->late_start) {
		l->late_start = 0;
		l->current_iid = ack->iid;
	}
		
	if (ack->iid > l->highest_iid_seen)
		l->highest_iid_seen = ack->iid;
	
	// Already closed and delivered, ignore message
	if (ack->iid < l->current_iid) {
		paxos_log_debug("Dropping accept_ack for already delivered iid: %u",
		ack->iid);
		return;
	}
	
	// We are late w.r.t the current iid, ignore message
	// (The instance received is too ahead and will overwrite something)
	if (ack->iid >= l->current_iid + carray_size(l->instances)) {
		paxos_log_debug("Dropping accept_ack for iid: %u, too far in future",
			ack->iid);
		return;
	}
	
	learner_update_instance(l, ack);
	
	struct instance* inst = learner_get_instance(l, ack->iid);
	if (instance_has_quorum(l, inst) && (inst->iid > l->highest_iid_closed))
		l->highest_iid_closed = inst->iid;
}

static void
initialize_instances(struct learner* l, int count)       //初始化实例
{
	int i;
	l->instances = carray_new(count);
	assert(l->instances != NULL);	
	for (i = 0; i < carray_size(l->instances); i++)
		carray_push_back(l->instances, instance_new(l->acceptors));
}

int
learner_has_holes(struct learner* l, iid_t* from, iid_t* to)       //检查空洞
{
	if (l->highest_iid_seen > l->current_iid + carray_count(l->instances)) {    //当前队列所有实例数量+当前iid都不如highest，说明有空洞
		*from = l->current_iid;
		*to = l->highest_iid_seen;
		return 1;
	}
	if (l->highest_iid_closed > l->current_iid) {
		*from = l->current_iid;
		*to = l->highest_iid_closed;
		return 1;
	}
	return 0;
}

struct learner*
learner_new(int acceptors)          //创建新的学习者
{
	struct learner* l;
	l = malloc(sizeof(struct learner));
	l->quorum = paxos_quorum(acceptors);
	l->acceptors = acceptors;
	l->current_iid = 1;
	l->highest_iid_seen = 1;
	l->highest_iid_closed = 1;
	l->late_start = !paxos_config.learner_catch_up;
	initialize_instances(l, paxos_config.learner_instances);   //根据配置文件中的值设置instance的数量并初始化
	return l;
}

void
learner_free(struct learner* l)        //释放learner
{
	int i;
	for (i = 0; i < carray_count(l->instances); i++)
		instance_free(carray_at(l->instances, i), l->acceptors);
	carray_free(l->instances);
	free(l);
}
