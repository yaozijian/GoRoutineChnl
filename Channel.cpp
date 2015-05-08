
#include "Channel.h"

static boost::mutex s_mtxManager;
static GoRoutineMgr *s_pManager = 0;

void GoRoutineMgr::go(GoRoutine::Go_Routine_Func fn){
	GoRoutineMgr* pManager = GoRoutineMgr::createInstance();
	if (pManager != 0){
		pManager->createRoutine(fn);
	}
}

GoRoutineMgr* GoRoutineMgr::createInstance(){

	boost::unique_lock<boost::mutex> lock(s_mtxManager);

	GoRoutineMgr* pManager = s_pManager;

	if (pManager == 0){
		pManager = new(std::nothrow) GoRoutineMgr;
		if ((pManager != 0) && (pManager->m_fiber_addr != 0)){
			s_pManager = pManager;
		}else{
			delete pManager;
			pManager = 0;
		}
	}

	return pManager;
}

void GoRoutineMgr::createRoutine(const GoRoutine::Go_Routine_Func& fn){

	// ����һ��ȥ��
	GoRoutine* pRoutine = new(std::nothrow) GoRoutine(fn);

	if (pRoutine == 0){
		return;
	}

	// ��������ģ��ȥ�̵��˳�
	void *fiber_addr = CreateFiber(0,GoRoutine::go,pRoutine);

	if (fiber_addr != 0){

		pRoutine->m_fiber_addr = fiber_addr;
		pRoutine->m_mgr = this;

		boost::unique_lock<boost::mutex> lock(m_mtxList);
		m_listRoutine.push_back(pRoutine);

		// ����ǵ�һ��goroutine,���������ȹ���
		if (m_listRoutine.size() == 1){
			lock.unlock();
			scheduleproc();
		}
	}else{
		delete pRoutine;
	}
}

//---------------------------- ִ�е��� --------------------------------------

GoRoutineMgr::GoRoutineMgr(){
	m_fiber_addr = ConvertThreadToFiber(this);
	m_toErase = 0;
}

GoRoutineMgr::~GoRoutineMgr(){
	if (m_fiber_addr != 0){
		DeleteFiber(m_fiber_addr);
	}
}

void GoRoutineMgr::schedule(GoRoutine* pToErase/*=0*/){
	m_toErase = pToErase;
	SwitchToFiber(this->m_fiber_addr);// �л��������˳�
}

void GoRoutineMgr::scheduleproc(){

	boost::random::mt19937 random_engine;
	random_engine.seed(GetTickCount());

	boost::unique_lock<boost::mutex> lock(m_mtxList);

	while(m_listRoutine.size() > 0){
		// ɾ���Ѿ���ɵ��˳�
		if (m_toErase != 0){
			this->remove(m_toErase);
			m_toErase = 0;
		}
		// ����л���ĳ�������˳�
		if (m_listRoutine.size() > 0){
			GoRoutine* pRoutine = this->randomSwitch(random_engine);
			lock.unlock();
			SwitchToFiber(pRoutine->m_fiber_addr);
			lock.lock();
		}
	}

	delete this;
}

// ɾ��һ��Goroutine
void GoRoutineMgr::remove(GoRoutine* pToErase){
	std::list<GoRoutine*>::iterator itor = m_listRoutine.begin();
	for(; itor != m_listRoutine.end(); ++itor){
		GoRoutine* pRoutine = *itor;
		if (pRoutine == pToErase){
			m_listRoutine.erase(itor);
			delete pRoutine;
			break;
		}
	}
}

// ����л���һ������״̬��goroutine
GoRoutine* GoRoutineMgr::randomSwitch(boost::random::mt19937& random_engine){

	int cnt = int(m_listRoutine.size());
	boost::random::uniform_int_distribution<> random_int(0,cnt);

	int waitcnt = 0,readycnt = 0;

	std::list<GoRoutine*>::iterator itor = m_listRoutine.begin();
	for(; itor != m_listRoutine.end(); itor++){
		GoRoutine* pRoutine = *itor;
		LONG state = pRoutine->m_state;
		// ���ڵȴ�״̬
		if ((state == GoRoutine::State_Wait4Read) || (state == GoRoutine::State_Wait4Write)){
			waitcnt++;
			if (waitcnt == cnt){
				throw "Deadlock: all goroutines are in waiting state";
			}
		}
		// ���ѡ���ھ���״̬��goroutine
		else if (state == GoRoutine::State_Ready){
			if (random_int(random_engine) % 3 == 0){
				InterlockedCompareExchange(&pRoutine->m_state,GoRoutine::State_Running,GoRoutine::State_Ready);
				return pRoutine;
			}else{
				readycnt++;
			}
		}
	}

	//====== ����Ĵ���û�����ѡ��һ������״̬��goroutine,���������� ======
	int readyidx = random_int(random_engine) % readycnt + 1;
	int curidx = 0;
	itor = m_listRoutine.begin();
	while(true){
		GoRoutine* pRoutine = *itor;
		LONG state = pRoutine->m_state;
		// ���ѡ���ھ���״̬��goroutine
		if ((state == GoRoutine::State_Ready) && (++curidx == readyidx)){
			InterlockedCompareExchange(&pRoutine->m_state,GoRoutine::State_Running,GoRoutine::State_Ready);
			return pRoutine;
		}else{
			itor++;
			if (itor == m_listRoutine.end()){
				itor = m_listRoutine.begin();
			}
		}
	}
}

//-----------------------------------------------------------------------------------

GoRoutine::GoRoutine(const Go_Routine_Func& fn){
	m_fn_go = fn;
	m_fiber_addr = 0;
	m_state = GoRoutine::State_Ready;
}

GoRoutine::~GoRoutine(){
	DeleteFiber(this->m_fiber_addr);
}

void WINAPI GoRoutine::go(void* p){
	((GoRoutine*)p)->start();
}

void GoRoutine::start(){
	InterlockedExchange(&m_state,State_Running);
	m_fn_go();
	m_mgr->schedule(this);
}

void GoRoutine::ready(){
	InterlockedExchange(&m_state,State_Ready);
}

void GoRoutine::yield(GoRoutine::YieldReason reason,std::list<Notify_Can_Read_Write_Func>& listFunc){

	GoRoutine* pRoutine = (GoRoutine*)GetFiberData();

	if (reason == Yield_4_Read){
		// ״̬: ���� --> �ȴ��̵��ɶ�
		InterlockedCompareExchange(&pRoutine->m_state,State_Wait4Read,State_Running);
	}else if (reason == Yield_4_Write){
		// ״̬: ���� --> �ȴ��̵���д
		InterlockedCompareExchange(&pRoutine->m_state,State_Wait4Write,State_Running);
	}

	// ���ڳ̵��ɶ�/��д��ʱ��֪ͨ��
	listFunc.push_back(boost::bind(&GoRoutine::ready,pRoutine));

	// ִ�е���
	pRoutine->m_mgr->schedule();
}
