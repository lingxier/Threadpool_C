#include "threadpool.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <unistd.h>
#include <Windows.h>

const int NUMBER = 2; //添加线程的个数
//任务结构体
typedef struct Task {
	//模板需要在编译时确定，但是void*能在运行时动态的传参
	void (*function)(void* arg);// 函数指针  参数void* 是泛型,可以兼容各种各样的地址类型
	void* arg;
} Task;

// 线程池结构体【固定的类似const值可以不用加锁，比如minNum、maxNum、queueCapacity，即初始化的值】
struct ThreadPool {
	//任务队列 【多线程访问】C++可以用std::queue<Task> taskQ;替代
	Task* taskQ;
	int queueCapacity; //容量
	int queueSize; // 当前任务个数
	int queueFront; // 队头 -> 取数据
	int queueRear; // 队尾 -> 放数据

	pthread_t managerID;  //管理者线程ID 不用手动释放，它是一个 值变量（不是指针），只存储线程的 ID，而不管理线程的生命周期。
	pthread_t* threadIDs; //工作的线程ID 有多个，指针指向数组

	//给线程池里面的线程指定范围
	int minNum; //最小线程数
	int maxNum; //最大线程数
	int busyNum; //忙的工作中的线程个数 【多线程访问：专用mutexBusy】
	int liveNum; //已创建存活的线程个数 【多线程访问，共用mutexpool】
	int exitNum; //要销毁的线程个数
	pthread_mutex_t mutexpool; //锁整个线程池
	//就是有个临界变量会经常发生数据变化，为了程序效率单独给这个变量申请个锁，而不是用公共的锁
	pthread_mutex_t mutexBusy; //锁busyNum变量
	pthread_cond_t notFull; // 任务队列是不是满了（没满）
	pthread_cond_t notEmpty; // 任务队列是不是空了（没空）
	int shutdown; //是不是要销毁线程池，销毁为1，不销毁为0
};

ThreadPool* threadPoolCreate(int min, int max, int queueSize) {
	//threadids作为指针，为pool申请空间，只是给threadids一个存放位置，ids并没有指向的空间，所以需要申请，否则为野指针
	ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
	do {
		//判断pool是否申请成功
		if (pool == NULL) {
			printf("malloc threadpool fail...\n");
			break;
		}

		pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * max);
		//判断threadIDs是否申请成功
		if (pool->threadIDs == NULL) {
			printf("malloc threadIDs fail...\n");
			break;
		}

		//memset(pool->threadIDs, 0, sizeof(pthread_t) * max);
		/*在 Linux（glibc）中，pthread_t 实际上是 unsigned long，所以 memset 可能不会出错。
		 *memset 逐字节填充 0，相当于初始化线程 ID 数组。
		但在其他 POSIX 线程实现（比如 macOS、某些 Unix 版本）中，pthread_t 可能是结构体，直接 memset 可能导致未定义行为。*/
		//memset(pool->threadIDs, 0, sizeof(pthread_t) * max);
		//初始化线程池
		for (int i = 0; i < max; i++) {
			pool->threadIDs[i] = pthread_t();  // 使用默认构造
		}
		pool->minNum = min;
		pool->maxNum = max;
		pool->busyNum = 0;
		pool->liveNum = min; //和最小个数相等
		pool->exitNum = 0;
		//返回0表示初始化成功
		if (pthread_mutex_init(&pool->mutexpool, NULL) != 0 || pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
			pthread_cond_init(&pool->notEmpty, NULL) != 0 || pthread_cond_init(&pool->notFull, NULL) != 0) {
			printf("mutex or condition init fail...\n");
			return 0;
		}
		//任务队列
		pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
		pool->queueCapacity = queueSize;
		pool->queueSize = 0;
		pool->queueFront = 0;
		pool->queueRear = 0;

		pool->shutdown = 0;

		//创建线程(地址，属性（默认），函数，参数)
		pthread_create(&pool->managerID, NULL, manager, pool);
		for (int i = 0; i < min; ++i) {
			pthread_create(&pool->threadIDs[i], NULL, worker, pool);
		}
		return pool;
	} while (0);
	//释放资源，如果循环异常退出，释放内存
	if (pool && pool->threadIDs) {
		free(pool->threadIDs);
	}
	if (pool && pool->taskQ) {
		free(pool->taskQ);
	}
	if (pool) {
		free(pool);
	}
	return NULL;
}

void* worker(void* arg) {
	ThreadPool* pool = (ThreadPool*)arg;
	while (1) {
		pthread_mutex_lock(&pool->mutexpool);
		// 【消费者】如果线程中没有被关闭且任务队列为空【多线程访问】条件变量不满足时解锁，等待条件变量满足时再加锁
		// 当前任务队列是否为空
		// 如果线程中没有被关闭且没有任务
		while (pool->queueSize == 0 && !pool->shutdown) {
			// 阻塞工作线程
			// wait先解锁pool->mutexpool，然后等待条件变量pool->notEmpty，条件变量有效后再上锁pool->mutexpool
			// pool->notEmpty是条件变量，记录哪些线程在等待任务
			// 【需要得到一个“非空”的条件变量】
			pthread_cond_wait(&pool->notEmpty, &pool->mutexpool);

			// 判断是不是要销毁线程
			// 使用 pthread_cond_wait 函数时，该函数会先解锁 mutex（互斥锁），
			// 以允许其他线程访问被保护的共享资源，然后它会将线程挂起，直到另一个线程唤醒它。
			if (pool->exitNum > 0) {
				pool->exitNum--; //不能放在if里面，哈哈，就是公司引导你辞职，但是现在你又不能辞职（因为你辞职后，公司人数小于一个最小人数），紧接着公司来了很多新任务，缺人，但此时 公司依然有exitNum个裁员计划
				if (pool->liveNum > pool->minNum) {
					//为了保证最小线程数，只有当存活的线程数大于最小线程数时才能销毁线程
					pool->liveNum--;
					pthread_mutex_unlock(&pool->mutexpool);
					threadExit(pool);
				}
			}
		}
		//判断线程池是否被关闭了
		if (pool->shutdown) {
			pthread_mutex_unlock(&pool->mutexpool);
			threadExit(pool);
		}
		// 从任务队列中取出一个任务
		//taskQ 本质是个队列，队列可以用链表也可以用数组，这里用数组
		Task task;
		task.function = pool->taskQ[pool->queueFront].function;
		task.arg = pool->taskQ[pool->queueFront].arg;
		// 移动头结点
		pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
		pool->queueSize--;
		// 解锁
		pthread_cond_signal(&pool->notFull);//唤醒添加任务的线程
		pthread_mutex_unlock(&pool->mutexpool);

		//printf("thread % ld start working...\n", pthread_self());
		printf("thread % lu start working...\n", GetCurrentThreadId());

		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum++;
		pthread_mutex_unlock(&pool->mutexBusy);
		task.function(task.arg); //通过函数指针调用函数
		//(*task.function)(task.arg);//通过解引用调用函数
		free(task.arg);
		task.arg = NULL;

		//printf("thread % ld end working...\n", pthread_self());
		printf("thread % lu end working...\n", GetCurrentThreadId());
		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum--;
		pthread_mutex_unlock(&pool->mutexBusy);
		//task.function(task.arg);
	}
	return NULL;
}

void* manager(void* arg) {
	ThreadPool* pool = (ThreadPool*)arg;
	while (!pool->shutdown) {
		// 每隔3s检测一次
		//sleep(3);
		Sleep(3*1000);

		// 取出线程池中任务的数量和当前线程的数量
		pthread_mutex_lock(&pool->mutexpool);
		int queueSize = pool->queueSize;//任务队列中的任务个数
		int liveNum = pool->liveNum;
		pthread_mutex_unlock(&pool->mutexpool);

		//取出忙的线程的数量
		pthread_mutex_lock(&pool->mutexBusy);
		int busyNum = pool->busyNum;
		pthread_mutex_unlock(&pool->mutexBusy);

		// 添加线程(任务的个数>存活的线程数）
		// 任务的个数>存活的线程个数 && 存活的线程数<最大线程数
		if (queueSize > liveNum && liveNum < pool->maxNum) {
			pthread_mutex_lock(&pool->mutexpool);
			int counter = 0;//计数器
			//因为这一块代码只有管理线程会调用，而管理线程只有一个
			for (int i = 0; i < pool->maxNum && counter < NUMBER && pool->liveNum < pool->maxNum; ++i) {
				//if (pool->threadIDs[i] == 0) { //线程ID为0说明线程没有被创建
				if (pthread_equal(pool->threadIDs[i], pthread_t())) { //线程ID为0说明线程没有被创建
					pthread_create(&pool->threadIDs[i], NULL, worker, pool);
					counter++;
					pool->liveNum++;
				}
			}
			pthread_mutex_unlock(&pool->mutexpool);
		}

		// 销毁线程(忙的线程*2 < 存活的线程数)
		// 忙的线程*2 < 存活的线程数 && 存活的线程 > 最小线程数
		if (busyNum * 2 < liveNum && liveNum > pool->minNum) {
			pthread_mutex_lock(&pool->mutexpool);
			pool->exitNum = NUMBER;//要销毁的线程个数 销毁过程放在worker里面
			pthread_mutex_unlock(&pool->mutexpool);
			//让工作的线程自杀
			for (int i = 0; i < NUMBER; ++i) {
				// 需要得到一个不是空的条件变量，说明工作线程可以继续工作
				pthread_cond_signal(&pool->notEmpty);
			}
		}
	}
	return NULL;
}

void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg) {
	pthread_mutex_lock(&pool->mutexpool);
	// 判断任务队列是否满了 【生产者】判断任务队列是否满了或者人为标志位是否关闭【多线程访问】条件变量不满足时解锁，等待条件变量满足时再加锁
	while (pool->queueSize == pool->queueCapacity && !pool->shutdown) {
		// 阻塞添加任务的线程，等待条件变量 notFull 满足的同时释放互斥锁 mutexpool，并阻塞当前线程，直到被其他线程唤醒。
		// 唤醒时，线程会重新获得 mutexpool 锁并继续执行。
		// 【需要得到一个“非满”的条件变量】
		pthread_cond_wait(&pool->notFull, &pool->mutexpool);
	}
	//当有线程等待时线程池的关闭标志 pool->shutdown 被设为 true，此时应唤醒所有等待的线程释放资源，但此线程不应再添加任务
	if (pool->shutdown) {
		pthread_mutex_unlock(&pool->mutexpool);
		return;
	}
	// 添加任务
	pool->taskQ[pool->queueRear].function = func;
	pool->taskQ[pool->queueRear].arg = arg;
	pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
	pool->queueSize++;
	// 任务队列不为空，唤醒线程池中的线程
	pthread_cond_signal(&pool->notEmpty);
	pthread_mutex_unlock(&pool->mutexpool);
}

int threadPoolBusyNum(ThreadPool* pool) {
	//【这里理解有问题】因为互斥锁锁的是线程, 而不是变量, 在任何一个没加解锁的线程里都可以在别的线程占有锁的时候, 读写变量
	//1.如果这里不加锁, 调用者会在任何情况下都直接读取后就能继续往下执行, 而不是阻塞等待已经占有锁的线程解锁
	//2.这就违背了互斥锁函数的设计初衷, 如果希望读取不受限制的话, 干脆就用读写锁好了, 这是C语言信任程序员的特性,自由度和危险
	//【新理解】1、互斥锁的作用是控制对共享资源的访问，而不是控制线程的执行。2、锁住的是资源，如果一个线程持有锁，其他线程在尝试获取锁时会被阻塞，直到锁被释放；锁住的不是线程，这意味着线程可以继续执行，只是不能同时访问被锁住的资源。3、如果某个线程没有加锁就直接访问共享资源，可能会导致数据竞争，这是不安全的。
	pthread_mutex_lock(&pool->mutexBusy);
	int busyNum = pool->busyNum;
	pthread_mutex_unlock(&pool->mutexBusy);
	return busyNum;
}

int threadPoolAliveNum(ThreadPool* pool) {
	pthread_mutex_lock(&pool->mutexpool);
	int liveNum = pool->liveNum;
	pthread_mutex_unlock(&pool->mutexpool);
	return liveNum;
}

//销毁某个线程
void threadExit(ThreadPool* pool) {
	pthread_t tid = pthread_self();//获取线程ID
	for (int i = 0; i < pool->maxNum; ++i) {
		//if (pool->threadIDs[i] == tid) { //找到要销毁的线程
		if (pthread_equal(pool->threadIDs[i], tid)) { //找到要销毁的线程
			//pool->threadIDs[i] = 0;	//不能赋值为0，因为pthread_t是一个结构体，赋值为0会导致线程ID丢失
			pool->threadIDs[i] = pthread_t();
			//printf("threadExit() called, % ld exiting...\n", tid);
			printf("threadExit() called, % lu exiting...\n", GetCurrentThreadId());
			break;
		}
	}
	pthread_exit(NULL);
}

int threadPoolDestroy(ThreadPool* pool) {
	if (pool == NULL) {
		return -1;
	}
	//关闭线程池
	pool->shutdown = 1;
	//唤醒阻塞的消费者线程
	for (int i = 0; i < pool->liveNum; ++i) {
		pthread_cond_signal(&pool->notEmpty);
	}
	//销毁管理者线程
	pthread_join(pool->managerID, NULL);
	// 释放堆内存
	if (pool->taskQ) {
		free(pool->taskQ);
	}
	if (pool->threadIDs) {
		free(pool->threadIDs);
	}
	//放在free的前面更好，但是也可以，free只是释放了指向内存(该内存可以分配给别的变量了)，
	//但是依然可以通过pool指针变量访问到这个内存
	pthread_mutex_destroy(&pool->mutexpool);
	pthread_mutex_destroy(&pool->mutexBusy);
	pthread_cond_destroy(&pool->notEmpty);
	pthread_cond_destroy(&pool->notFull);
	free(pool);
	pool = NULL;
	return 0;
}
