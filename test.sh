#!/bin/sh

## no UTF-8
LC_CTYPE=C

## params
PARAM_MAX_MESSAGE_LENGTH=75
PARAM_FAGMENTS_AT_A_TIME=10
PARAM_RECEIPT_EXPIRATION_TIME_MS=1000
PARAMS="$PARAM_MAX_MESSAGE_LENGTH $PARAM_FAGMENTS_AT_A_TIME $PARAM_RECEIPT_EXPIRATION_TIME_MS"
CMD_PEER=./test-peer
NET_SOCKET=test-net-socket

## procedures
randomNumber() {
  local max=$1
  max=$((max+1))
  local r=$(od -An -N2 -i /dev/random)
  echo `expr $r % $max`
}
randomNumberInRange() {
  local min=$1
  local max=$2
  echo $((min + $(randomNumber $((max-min)))))
}
randomString() {
  local len=$1
  cat /dev/urandom | LC_CTYPE=C tr -cd "[:alnum:]" | fold -w $len | head -1
}
generateMessage() {
  local len=$1
  echo "M $len $(randomString $len)"
}
generateMessages() {
  local num=$1
  local lenMin=$2
  local lenMax=$3
  while [ $num -gt 0 ]; do
    generateMessage $(randomNumberInRange $lenMin $lenMax)
    num=$((num-1))
  done
}
generateTestInput() {
  generateMessages 10 30 100
  generateMessages 10 1000 2000
  generateMessages 10 50 5000
  echo "E"
}

## generate input
generateTestInput > test-in1.txt
generateTestInput > test-in2.txt

## run peer simulation
rm -f test-db1.sqlite test-db2.sqlite $NET_SOCKET
$CMD_PEER 5 7 test-db1.sqlite $NET_SOCKET C $PARAMS < test-in1.txt > test-out1.txt &
$CMD_PEER 7 5 test-db2.sqlite $NET_SOCKET L $PARAMS < test-in2.txt > test-out2.txt &

## wait for the peers to finish
FAIL=0
for job in `jobs -p`
do
  wait $job || let "FAIL+=1"
done

if [ "$FAIL" -ne "0" ]; then
  echo "FAILURE: $FAIL process(es) failed"
  exit 1
fi

echo "SUCCESS!"
