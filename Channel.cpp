
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

	// 创建一个去程
	GoRoutine* pRoutine = new(std::nothrow) GoRoutine(fn);

	if (pRoutine == 0){
		return;
	}

	// 创建用以模拟去程的纤程
	void *fiber_addr = CreateFiber(0,GoRoutine::go,pRoutine);

	if (fiber_addr != 0){

		pRoutine->m_fiber_addr = fiber_addr;
		pRoutine->m_mgr = this;

		boost::unique_lock<boost::mutex> lock(m_mtxList);
		m_listRoutine.push_back(pRoutine);

		// 如果是第一个goroutine,则启动调度过程
		if (m_listRoutine.size() == 1){
			lock.unlock();
			scheduleproc();
		}
	}else{
		delete pRoutine;
	}
}

//---------------------------- 执行调度 --------------------------------------

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
	SwitchToFiber(this->m_fiber_addr);// 切换到调度纤程
}

void GoRoutineMgr::scheduleproc(){

	boost::random::mt19937 random_engine;
	random_engine.seed(GetTickCount());

	boost::unique_lock<boost::mutex> lock(m_mtxList);

	while(m_listRoutine.size() > 0){
		// 删除已经完成的纤程
		if (m_toErase != 0){
			this->remove(m_toErase);
			m_toErase = 0;
		}
		// 随机切换到某个其他纤程
		if (m_listRoutine.size() > 0){
			GoRoutine* pRoutine = this->randomSwitch(random_engine);
			lock.unlock();
			SwitchToFiber(pRoutine->m_fiber_addr);
			lock.lock();
		}
	}

	delete this;
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

// 随机切换到一个就绪状态的goroutine
GoRoutine* GoRoutineMgr::randomSwitch(boost::random::mt19937& random_engine){

	int cnt = int(m_listRoutine.size());
	boost::random::uniform_int_distribution<> random_int(0,cnt);

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
