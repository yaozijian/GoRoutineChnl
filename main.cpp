
#include "Channel.h"
#include <stdio.h>


static void gomain();

int main(){
	GoRoutineMgr::go(boost::bind(gomain));
	return 0;
}

static void send_routine(GoChannel<int>* pChnl){

	boost::random::mt19937 random_engine;
	boost::random::uniform_int_distribution<> random_int(1,100);
	random_engine.seed(GetTickCount());

	bool op_ok;

	while(true){
		int val = random_int(random_engine);
		pChnl->write(val,op_ok);
		if (op_ok){
			printf("���ֵ�Ѿ�д��: %d\n",val);
		}
		else{
			printf("=== д��ʧ�� ===\n");
			break;
		}
	}
}

static void gomain(){

	GoChannel<int>* pChnl = new GoChannel<int>();

	GoRoutineMgr::go(boost::bind(send_routine,pChnl));

	bool op_ok;
	int val;

	for(int idx = 0; idx < 5; ++idx){
		pChnl->read(val,op_ok);
		//pChnl->write(val,op_ok);
		printf("== ȡ�����ֵ: %d\n\n",val);
	}

	printf("===== �رճ̵� =====\n");

	pChnl->close();

	printf("===== OK =====\n");
}
