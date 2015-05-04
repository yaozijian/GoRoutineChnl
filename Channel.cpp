
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

	// 去程管理器个数还没有达到最大值,则新建去程管理器
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
		// 在已有的去程管理器中增加一个去程
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

	// 创建一个去程
	GoRoutine* pRoutine = new(std::nothrow) GoRoutine(fn);

	if (pRoutine == 0){
		return (GoRoutine*)0;
	}

	// 创建用以模拟去程的纤程
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

//---------------------------- 执行调度 --------------------------------------

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

// 删除一个Goroutine
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

// 删除Routine管理器
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
		// 所有去程管理器已经销毁,则程序退出
		lock.unlock();
		ExitProcess(0);
	}else{
		// 当前去程管理器已经销毁,退出所使用的操作系统线程
		lock.unlock();
		ExitThread(0);
	}
}

// 随机切换到一个就绪状态的goroutine
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
		// 处于等待状态
		if ((state == GoRoutine::State_Wait4Read) || (state == GoRoutine::State_Wait4Write)){
			waitcnt++;
			if (waitcnt == cnt){
				throw "Deadlock: all goroutines are in waiting state";
			}
		}
		// 随机选择处于就绪状态的goroutine
		else if (state == GoRoutine::State_Ready){
			if (random_int(random_engine) % 3 == 0){
				InterlockedCompareExchange(&pRoutine->m_state,GoRoutine::State_Running,GoRoutine::State_Ready);
				return pRoutine;
			}else{
				readycnt++;
			}
		}
	}

	//====== 上面的代码没有随机选中一个就绪状态的goroutine,接下来继续 ======
	int readyidx = random_int(random_engine) % readycnt + 1;
	int curidx = 0;
	itor = m_listRoutine.begin();
	while(true){
		GoRoutine* pRoutine = *itor;
		LONG state = pRoutine->m_state;
		// 随机选择处于就绪状态的goroutine
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
		// 状态: 运行 --> 等待程道可读
		InterlockedCompareExchange(&pRoutine->m_state,State_Wait4Read,State_Running);
	}else if (reason == Yield_4_Write){
		// 状态: 运行 --> 等待程道可写
		InterlockedCompareExchange(&pRoutine->m_state,State_Wait4Write,State_Running);
	}

	// 请在程道可读/可写的时候通知我
	listFunc.push_back(boost::bind(&GoRoutine::ready,pRoutine));

	// 执行调度
	pRoutine->m_mgr->schedule();
}
