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


#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAX_SIZE_RECORD (8*1024)
#define MAX_RECORDS (4*1024)

struct storage
{
	int acceptor_id;
	acceptor_record *records[MAX_RECORDS];
};

struct storage*
storage_open(int acceptor_id)            //打开storage
{
	struct storage* s = malloc(sizeof(struct storage));  //分配空间，并且给每个records分配空间
	assert(s != NULL);
	memset(s, 0, sizeof(struct storage));
	
	int i;
	for (i=0; i<MAX_RECORDS; ++i){
		s->records[i] = (acceptor_record *) malloc(MAX_SIZE_RECORD);
		assert(s->records[i] != NULL);
		s->records[i]->iid = 0;
	}

	s->acceptor_id = acceptor_id;

	return s;
}

int
storage_close(struct storage* s)      //关闭storage，释放空间
{
	int i;
	for (i=0; i<MAX_RECORDS; ++i){
		free(s->records[i]);
	}
	
	free(s);
	
	return 0;
}

void
storage_tx_begin(struct storage* s)
{
	return;
}

void
storage_tx_commit(struct storage* s)
{
	return;
}

acceptor_record* 
storage_get_record(struct storage* s, iid_t iid)        //获取相应的record
{
	acceptor_record* record_buffer = s->records[iid % MAX_RECORDS];
	if (iid < record_buffer->iid){                                   //如果iid比record中已经存在的iid小，说明实例过期了
		fprintf(stderr, "instance too old %d: current is %d", iid, record_buffer->iid);
		exit(1);
	}
	if (iid == record_buffer->iid){                 //取出record
		return record_buffer;
	} else {                                              //不存在record
		return NULL;
	}
}

acceptor_record*
storage_save_accept(struct storage* s, accept_req * ar)     //保存accept回复，accept_ack
{
	acceptor_record* record_buffer = s->records[ar->iid % MAX_RECORDS];
	
	//Store as acceptor_record (== accept_ack)
	record_buffer->acceptor_id = s->acceptor_id;
	record_buffer->iid = ar->iid;
	record_buffer->ballot = ar->ballot;
	record_buffer->value_ballot = ar->ballot;
	record_buffer->is_final = 0;
	record_buffer->value_size = ar->value_size;
	memcpy(record_buffer->value, ar->value, ar->value_size);
	
	return record_buffer;
}

acceptor_record*
storage_save_prepare(struct storage* s, prepare_req* pr, acceptor_record* rec)     //保存prepare请求
{
	acceptor_record* record_buffer = s->records[pr->iid % MAX_RECORDS];
	
	//No previous record, create a new one
	if (rec == NULL) {
		//Record does not exist yet
		rec = record_buffer;
		rec->acceptor_id = s->acceptor_id;
		rec->iid = pr->iid;
		rec->ballot = pr->ballot;
		rec->value_ballot = 0;
		rec->is_final = 0;
		rec->value_size = 0;
	} else {
		//Record exists, just update the ballot
		rec->ballot = pr->ballot;
	}
	
	return record_buffer;
}

acceptor_record*
storage_save_final_value(struct storage* s, char* value, size_t size,       //存储最终值 
	iid_t iid, ballot_t b)
{
	acceptor_record* record_buffer = s->records[iid % MAX_RECORDS];
	
	//Store as acceptor_record (== accept_ack)
	record_buffer->iid = iid;
	record_buffer->ballot = b;
	record_buffer->value_ballot = b;
	record_buffer->is_final = 1;
	record_buffer->value_size = size;
	memcpy(record_buffer->value, value, size);
	
		return record_buffer;
}
