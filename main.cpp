// ProConsumer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <stdio.h>
//#include <unistd.h>	//Linux下的头文件
#include <Windows.h>	//Windows下的头文件
#include <stdlib.h>
#include <pthread.h>
#include "threadpool.h"

//任务函数
void taskFunc(void* arg)
{
	if (arg == NULL) {	//bug，本来是要判断arg是否为NULL的，但是没有判断，arg会是个野指针
		printf("Error: arg is NULL\n");
		return;
	}
	//如果上面arg没有判断，arg会是个野指针
	int* num = (int*)arg;
	//*num = 100;//如果arg和i的地址是一样的，当这里修改num值即修改了i值，会影响main中for循环次数
	//bug，不能写成%d,因为pthread_self()类型是pthread_t（不同平台可能是unsigned long、结构体、指针等）
	//printf("thread %lu is working, number = %d\n", pthread_self(), num);
	DWORD thread_id = GetCurrentThreadId();//在 Windows 系统中（Windows 线程 ID 由 GetCurrentThreadId() 获取）
	printf("thread %lu is working, number = %d\n", thread_id, *num);
	//sleep(1);
	Sleep(1 * 1000);
}

int main()
{
	//创建线程池(最小线程数3，最大线程数10，队列最大100)
	ThreadPool* pool = threadPoolCreate(3, 10, 100);
	for (int i = 0; i < 100; i++) {
		//int* num = &i;//错误：为啥不能用局部变量放进线程函数，因为线程函数可能在主线程之后执行，局部变量可能被销毁
		//1、数据竞争：i 是一个局部变量，在每次循环迭代中都会被修改。当多个线程同时访问和修改 i 时，会导致数据竞争，进而导致未定义行为。
		//2、悬空指针：当 i 的生命周期结束后，num 仍然指向 i 的地址，这会导致悬空指针问题。如果在 i 的生命周期结束后访问 num，会导致程序崩溃或其他不可预知的行为。
		//3、任务参数错误：由于 num 指向的是同一个 i，所有线程任务接收到的参数都是相同的。这意味着所有线程都会处理相同的任务参数，而不是预期的不同参数。
		//4、指针作为函数参数传进线程函数可能被修改影响循环
		int* num = (int*)malloc(sizeof(int));
		*num = i;
		//添加任务 多线程环境传递局部变量地址会导致未定义行为。应动态分配内存给 num,可以确保每个任务都有自己独立的内存空间。
		threadPoolAdd(pool, taskFunc, num);
	}
	//sleep(30);	Linux下的休眠函数
	Sleep(22 * 1000);//Windows下的休眠函数

	//销毁整个线程池
	threadPoolDestroy(pool);
	return 0;
}

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
