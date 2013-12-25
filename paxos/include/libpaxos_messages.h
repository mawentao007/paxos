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


#ifndef _LIBPAXOS_MESSAGES_H_
#define _LIBPAXOS_MESSAGES_H_

#include "paxos.h"
#include <stdlib.h>

/*
    Paxos message types
*/

typedef enum pax_msg_code_e {
	prepare_reqs=1,     //Phase 1a, P->A
	prepare_acks=2,     //Phase 1b, A->P
	accept_reqs=4,      //Phase 2a, P->A
	accept_acks=8,      //Phase 2b, A->L
	repeat_reqs=16,     //For progress, L -> A
	submit=32,          //Clients to leader
	leader_announce=64, //Oracle to proposers
	alive_ping=65       //Proposers to oracle
} paxos_msg_code;

typedef struct paxos_msg_t {
	size_t data_size; //Size of 'data' in bytes
	paxos_msg_code type;
	char data[0];
}  __attribute__((packed)) paxos_msg;
#define PAXOS_MSG_SIZE(M) (M->data_size + sizeof(paxos_msg))

/* 
	Paxos protocol messages
*/
//Phase 1a, prepare request     p-a
typedef struct prepare_req_t {
	iid_t iid;
	ballot_t ballot;
} prepare_req;
#define PREPARE_REQ_SIZE(M) (sizeof(prepare_req))

//Phase 1b, prepare acknowledgement      a-p
typedef struct prepare_ack_t {
	int acceptor_id;
	iid_t iid;
	ballot_t ballot;
	ballot_t value_ballot;
	size_t value_size;
	char value[0];
} prepare_ack;
#define PREPARE_ACK_SIZE(M) (M->value_size + sizeof(prepare_ack)) 

//Phase 2a, accept request        p-a
typedef struct accept_req_t {
	iid_t iid;
	ballot_t ballot;
	size_t value_size;
	char value[0];
} accept_req;
#define ACCEPT_REQ_SIZE(M) (M->value_size + sizeof(accept_req))

//Phase 2b, accept acknowledgement   a-l
typedef struct accept_ack_t {        
	int acceptor_id;
	iid_t iid;
	ballot_t ballot;
	ballot_t value_ballot;
	short int is_final;
	size_t value_size;
	char value[0];
} accept_ack;
#define ACCEPT_ACK_SIZE(M) (M->value_size + sizeof(accept_ack))

//Storage type
typedef accept_ack acceptor_record;

#endif
