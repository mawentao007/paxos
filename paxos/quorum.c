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


#include "paxos.h"
#include "quorum.h"
#include <stdlib.h>


void
quorum_init(struct quorum* q, int acceptors)
{
	q->acceptors = acceptors;                               //acceptor数量
	q->quorum = paxos_quorum(acceptors);              //多数派数量
	q->acceptor_ids = malloc(sizeof(int) * q->acceptors);
	quorum_clear(q);
}

void
quorum_clear(struct quorum* q)            //多数派中acceptor相应位置0
{
	int i;
	q->count = 0;
	for (i = 0; i < q->quorum; ++i)
		q->acceptor_ids[i] = 0;
}

void
quorum_destroy(struct quorum* q)
{
	free(q->acceptor_ids);
}

int
quorum_add(struct quorum* q, int id)        //多数派中添加
{
	if (q->acceptor_ids[id] == 0) {       //如果相应acceptor还未添加
		q->count++;                   //添加，返回1
		q->acceptor_ids[id] = 1;
		return 1;
	}
	return 0;                                //否则返回0，表示之前已经存在了
}

int
quorum_reached(struct quorum* q)        //判断是否达到多数派
{
	return (q->count >= q->quorum);
}
