#!/bin/sh
./sample/acceptor 0 ../paxos.conf  & 
./sample/acceptor 1 ../paxos.conf  &
./sample/acceptor 2 ../paxos.conf  &
./sample/learner ../paxos.conf & 
./sample/client 127.0.0.1:5550 1 & 


