CC = cc 
filename = new_alarm_cond.c
output = alarm

all: main run


main:
	${CC} ${filename} -D_POSIX_PTHREAD_SEMANTICS -lpthread -o ${output}

debug:
	${CC} ${filename} -D_POSIX_PTHREAD_SEMANTICS -lpthread -DDEBUG -o ${output}

run:
	./${output}

clean:
	rm -f ${output}