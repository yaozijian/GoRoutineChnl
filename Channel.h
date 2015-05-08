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
		Yield_4_Read,	// ��Ϊ��ȡ����������
		Yield_4_Write,	// ��Ϊд�����������
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

	Go_Routine_Func m_fn_go;	// Ҫִ�еĺ���
	void* m_fiber_addr;			// ��Ӧ��Windows�˳̵�ַ
	GoRoutineMgr* m_mgr;		// ������ȥ�̹�����
};

//--------------------------------------------------------------------------------------------

class GoRoutineMgr{
public:

	static void setGOMAXPROCS(uint32_t);	// ��������ö��ٸ��߳���ִ��ȥ��
	static void go(GoRoutine::Go_Routine_Func);	// �½�һ��ȥ��
	void schedule(GoRoutine* p = 0);	// ִ��ȥ�̵���

private:

	static GoRoutineMgr* createInstance();
	void createRoutine(const GoRoutine::Go_Routine_Func& fn);

	GoRoutineMgr();
	~GoRoutineMgr();
	void scheduleproc();

	void remove(GoRoutine* pToErase);
	GoRoutine* randomSwitch(boost::random::mt19937& random_engine);

	std::list<GoRoutine*> m_listRoutine;			// �������������ȥ���б�
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
		// �����ݿɶ�
		retVal = m_listData.front();
		m_listData.pop_front();
		isOK = true;
		notifyCanReadWrite(m_listWait4Write);
	}else if (!m_closed){
		// �����ݿɶ�
		notifyCanReadWrite(m_listWait4Write);
		if (bWait){
			lockMe.unlock();
			GoRoutine::yield(GoRoutine::Yield_4_Read,m_listWait4Read);
			lockMe.lock();
			goto begin_read;
		}
	}else{
		// �̵��Ѿ��ر�
	}
}

template<typename ItemType>
void GoChannel<ItemType>::write(const ItemType& itemVal,bool& isOK,bool bWait/*=true*/){

	boost::mutex::scoped_lock lockMe(m_mtxReadWrite);

	isOK = false;

begin_write:
	if (m_closed){
		// �̵��Ѿ��ر�,����д��
		return;
	}else if (m_listData.size() < m_capacity){
		// �����㹻,ֱ��д��
		m_listData.push_back(itemVal);
		isOK = true;
		notifyCanReadWrite(m_listWait4Read);
		return;
	}else if (m_capacity == 0){
		// �ǻ���̵�: ֻ���еȴ���ȡ��goroutineʱ�ſ���д��,д������̻��ѵȴ���ȡ���ݵ�goroutine����ȡ����
		if (m_listWait4Read.size()>0){
			m_listData.push_back(itemVal);
			isOK = true;
			notifyCanReadWrite(m_listWait4Read);
			return;
		}
	}

	// �ȴ��п��л�����������goroutine�����ȡ����
	if (bWait){
		notifyCanReadWrite(m_listWait4Read);
		lockMe.unlock();
		GoRoutine::yield(GoRoutine::Yield_4_Write,m_listWait4Write);
		lockMe.lock();
		goto begin_write;
	}else{
		// ����д��,�ֲ��ȴ�����д��
	}
}

template<typename ItemType>
void GoChannel<ItemType>::close(){
	boost::mutex::scoped_lock lockMe(m_mtxReadWrite);
	// ���ùرձ�־,�����ȥ���ڵȴ���ȡ/д��,֪ͨ���Զ�ȡ/д����(���س̵��Ѿ��رմ���)
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
