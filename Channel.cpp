
#include "Channel.h"
#include <boost/random.hpp>

boost::mutex s_mtxManager;
std::vector<GoRoutineMgr*> GoRoutineMgr::s_vecManager;
uint32_t GoRoutineMgr::s_maxManager = 1;
uint32_t GoRoutineMgr::s_nxtManager = 0;

void GoRoutineMgr::setGOMAXPROCS(uint32_t maxCnt){
	boost::unique_lock<boost::mutex> lock(s_mtxManager);
	if (maxCnt > 0){
		uint32_t prev = s_maxManager;
		s_maxManager = maxCnt;
		if ((s_maxManager < prev) && (s_nxtManager > s_maxManager - 1)){
			s_nxtManager = s_maxManager - 1;
		}
	}
}

void GoRoutineMgr::go(GoRoutine::Go_Routine_Func fn){

	boost::unique_lock<boost::mutex> lock(s_mtxManager);

	// ȥ�̹�����������û�дﵽ���ֵ,���½�ȥ�̹�����
	if (s_vecManager.size() < s_maxManager){

		GoRoutineMgr* pManager = new(std::nothrow) GoRoutineMgr;

		if (pManager != 0){

			s_vecManager.push_back(pManager);

			if (s_vecManager.size() == 1){
				GoRoutine* pRoutine = pManager->start(fn);
				if (pRoutine != 0){
					lock.unlock();
					pRoutine->start();
				}
			}else{
				boost::thread managerThread(&GoRoutineMgr::start,pManager,fn);
			}
		}
	}else{
		// �����е�ȥ�̹�����������һ��ȥ��
		GoRoutineMgr* pManager = s_vecManager[s_nxtManager];
		s_nxtManager = (s_nxtManager + 1) % s_maxManager;
		pManager->add(fn);
	}
}

GoRoutine* GoRoutineMgr::start(const GoRoutine::Go_Routine_Func& fn){
	return this->create(fn,false);
}

void GoRoutineMgr::add(const GoRoutine::Go_Routine_Func& fn){
	this->create(fn,true);
}

GoRoutine* GoRoutineMgr::create(const GoRoutine::Go_Routine_Func& fn,bool newFiber){

	// ����һ��ȥ��
	GoRoutine* pRoutine = new(std::nothrow) GoRoutine(fn);

	if (pRoutine == 0){
		return (GoRoutine*)0;
	}

	// ��������ģ��ȥ�̵��˳�
	void* fiber_addr;

	if (newFiber){
		fiber_addr = CreateFiber(0,GoRoutine::go,pRoutine);
	}else{
		fiber_addr = ConvertThreadToFiber(pRoutine);
	}

	if (fiber_addr != 0){
		pRoutine->m_fiber_addr = fiber_addr;
		pRoutine->m_mgr = this;
		m_mtxList.lock();
		m_listRoutine.push_back(pRoutine);
		m_mtxList.unlock();
		return pRoutine;
	}else{
		delete pRoutine;
		return (GoRoutine*)0;
	}
}

//---------------------------- ִ�е��� --------------------------------------

void GoRoutineMgr::schedule(GoRoutine* pToErase/*=0*/){
	boost::unique_lock<boost::mutex> lock(m_mtxList);
	if (pToErase != 0){
		this->remove(pToErase);
	}
	if (m_listRoutine.empty()){
		lock.unlock();
		this->deleteMe();
	}else{
		GoRoutine* pRoutine = this->randomSwitch();
		lock.unlock();
		SwitchToFiber(pRoutine->m_fiber_addr);
	}
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

// ɾ��Routine������
void GoRoutineMgr::deleteMe(){

	boost::unique_lock<boost::mutex> lock(s_mtxManager);

	std::vector<GoRoutineMgr*>::iterator itor = s_vecManager.begin();

	for(; itor != s_vecManager.end(); ++itor){
		if (*itor == this){
			s_vecManager.erase(itor);
			break;
		}
	}

	delete this;

	if (s_vecManager.empty()){
		// ����ȥ�̹������Ѿ�����,������˳�
		lock.unlock();
		ExitProcess(0);
	}else{
		// ��ǰȥ�̹������Ѿ�����,�˳���ʹ�õĲ���ϵͳ�߳�
		lock.unlock();
		ExitThread(0);
	}
}

// ����л���һ������״̬��goroutine
GoRoutine* GoRoutineMgr::randomSwitch(){

	int cnt = int(m_listRoutine.size());
	boost::random::mt19937 random_engine;
	boost::random::uniform_int_distribution<> random_int(0,cnt);
	random_engine.seed(GetTickCount());

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
