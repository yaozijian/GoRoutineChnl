#ifndef _Go_Channel_H_
#define _Go_Channel_H_

#ifndef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WIN2K
#endif

#include <Windows.h>
#include <stdint.h>
#include <list>
#include <vector>
#include <boost/thread.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/random.hpp>

class GoRoutineMgr;

class GoRoutine{

	friend class GoRoutineMgr;

public:

	typedef boost::function<void()> Go_Routine_Func;
	typedef boost::function<void()> Notify_Can_Read_Write_Func;

	enum YieldReason{
		Yield_4_Read,	// 因为读取操作而阻塞
		Yield_4_Write,	// 因为写入操作而阻塞
	};

	static void yield(YieldReason,std::list<Notify_Can_Read_Write_Func>&);

private:

	GoRoutine(const Go_Routine_Func&);
	~GoRoutine();

	static void WINAPI go(void*);
	void start();
	void ready();

	enum{
		State_Ready,
		State_Running,
		State_Wait4Read,
		State_Wait4Write
	};

	volatile LONG m_state;

	Go_Routine_Func m_fn_go;	// 要执行的函数
	void* m_fiber_addr;			// 对应的Windows纤程地址
	GoRoutineMgr* m_mgr;		// 所属的去程管理器
};

//--------------------------------------------------------------------------------------------

class GoRoutineMgr{
public:

	static void setGOMAXPROCS(uint32_t);	// 设置最多用多少个线程来执行去程
	static void go(GoRoutine::Go_Routine_Func);	// 新建一个去程
	void schedule(GoRoutine* p = 0);	// 执行去程调度

private:

	static GoRoutineMgr* createInstance();
	void createRoutine(const GoRoutine::Go_Routine_Func& fn);

	GoRoutineMgr();
	~GoRoutineMgr();
	void scheduleproc();

	void remove(GoRoutine* pToErase);
	GoRoutine* randomSwitch(boost::random::mt19937& random_engine);

	std::list<GoRoutine*> m_listRoutine;			// 本管理器管理的去程列表
	boost::mutex m_mtxList;

	GoRoutine* m_toErase;
	void* m_fiber_addr;
};

//--------------------------------------------------------------------------------------------

template<typename ItemType>
class GoChannel{

public:

	GoChannel(uint32_t capacity = 0);
	~GoChannel();

	void read(ItemType& readTo,bool& op_ok,bool bWait=true);
	void write(const ItemType& itemVal,bool& op_ok,bool bWait=true);
	void close();

private:

	void notifyCanReadWrite(std::list<GoRoutine::Notify_Can_Read_Write_Func>& waitList);

	uint32_t m_capacity;
	bool m_closed;

	std::list<ItemType> m_listData;

	std::list<GoRoutine::Notify_Can_Read_Write_Func> m_listWait4Read;
	std::list<GoRoutine::Notify_Can_Read_Write_Func> m_listWait4Write;

	boost::mutex m_mtxReadWrite;
};

//--------------------------------------------------------------------------------------------

template<typename ItemType>
GoChannel<ItemType>::GoChannel(uint32_t capacity/*=0*/){
	m_capacity = capacity;
	m_closed = false;
}

template<typename ItemType>
GoChannel<ItemType>::~GoChannel(){
}

template<typename ItemType>
void GoChannel<ItemType>::read(ItemType& retVal,bool& isOK,bool bWait/*=true*/){

	boost::mutex::scoped_lock lockMe(m_mtxReadWrite);

	isOK = false;

begin_read:
	if (m_listData.size() > 0){
		// 有数据可读
		retVal = m_listData.front();
		m_listData.pop_front();
		isOK = true;
		notifyCanReadWrite(m_listWait4Write);
	}else if (!m_closed){
		// 无数据可读
		notifyCanReadWrite(m_listWait4Write);
		if (bWait){
			lockMe.unlock();
			GoRoutine::yield(GoRoutine::Yield_4_Read,m_listWait4Read);
			lockMe.lock();
			goto begin_read;
		}
	}else{
		// 程道已经关闭
	}
}

template<typename ItemType>
void GoChannel<ItemType>::write(const ItemType& itemVal,bool& isOK,bool bWait/*=true*/){

	boost::mutex::scoped_lock lockMe(m_mtxReadWrite);

	isOK = false;

begin_write:
	if (m_closed){
		// 程道已经关闭,不能写入
		return;
	}else if (m_listData.size() < m_capacity){
		// 容量足够,直接写入
		m_listData.push_back(itemVal);
		isOK = true;
		notifyCanReadWrite(m_listWait4Read);
		return;
	}else if (m_capacity == 0){
		// 非缓冲程道: 只有有等待读取的goroutine时才可以写入,写入后立刻唤醒等待读取数据的goroutine来读取数据
		if (m_listWait4Read.size()>0){
			m_listData.push_back(itemVal);
			isOK = true;
			notifyCanReadWrite(m_listWait4Read);
			return;
		}
	}

	// 等待有空闲缓冲区或者有goroutine请求读取数据
	if (bWait){
		notifyCanReadWrite(m_listWait4Read);
		lockMe.unlock();
		GoRoutine::yield(GoRoutine::Yield_4_Write,m_listWait4Write);
		lockMe.lock();
		goto begin_write;
	}else{
		// 不能写入,又不等待可以写入
	}
}

template<typename ItemType>
void GoChannel<ItemType>::close(){
	boost::mutex::scoped_lock lockMe(m_mtxReadWrite);
	// 设置关闭标志,如果有去程在等待读取/写入,通知可以读取/写入了(返回程道已经关闭错误)
	if (!m_closed){
		m_closed = true;
		notifyCanReadWrite(m_listWait4Read);
		notifyCanReadWrite(m_listWait4Write);
	}
}

template<typename ItemType>
void GoChannel<ItemType>::notifyCanReadWrite(std::list<GoRoutine::Notify_Can_Read_Write_Func>& waitList){
	while (waitList.size() > 0){
		GoRoutine::Notify_Can_Read_Write_Func& fnNotify = waitList.front();
		fnNotify();
		waitList.pop_front();
	}
}

#endif
