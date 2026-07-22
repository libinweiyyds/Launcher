#pragma once


// DLL导出/导入宏
//
// 在编译 ipc.dll 时，需要定义 IPC_EXPORTS，表示导出函数：
//     __declspec(dllexport)
//
// 在使用 ipc.dll 的程序中，不定义 IPC_EXPORTS，表示导入函数：
//     __declspec(dllimport)
#ifdef IPC_EXPORTS
#define IPC_API __declspec(dllexport)
#else
#define IPC_API __declspec(dllimport)
#endif



// 保证C接口导出
//
// 因为DLL内部使用C++实现，如果不加extern "C"，
// C++编译器会对函数名进行名称修饰(name mangling)，
// 导致其他语言或C程序无法通过函数名调用。
#ifdef __cplusplus
extern "C"
{
#endif



	/**
	 * 对外隐藏内部实现。 实际内部保存的是IPCChannel对象指针，
	 * 但调用者不需要知道具体结构。
	 * 使用方式：IPC_HANDLE handle = ipc_client_connect("Service");
	 */
	typedef void* IPC_HANDLE;



	/**
	 * @brief  创建IPC服务端
	 * @param name  name：服务名称，例如："MyService"  底层实际对应Windows Named Pipe： \\.\pipe\MyService
	 * @return 成功：IPC_HANDLE 失败：nullptr
	 */
	IPC_API IPC_HANDLE ipc_server_create(
		const char* name
	);




	/**
	 * @brief  根据服务名称连接已经存在的IPC服务。
	 * @param name  服务名称，需要与服务端创建时一致。
	 * @return 成功：IPC_HANDLE 失败：nullptr
	 */
	IPC_API IPC_HANDLE ipc_client_connect(
		const char* name
	);


 
	
	/**
	 * @brief  通过当前IPC连接发送数据。
	 * @param handle   IPC连接句柄
	 * @param data     待发送的数据缓冲区
	 * @param size     数据长度（字节）
	 * @return  成功：发送字节数    失败：-1
	 */
	IPC_API int ipc_send(
		IPC_HANDLE handle,
		const void* data,
		int size
	);




	/**
	 * @brief  从IPC连接读取数据。
	 * @param handle   IPC连接句柄
	 * @param buffer   接收缓冲区
	 * @param size     缓冲区大小
	 * @return   >0 : 实际接收字节数   0 : 客户端断开    -1 : 接收失败
	 */
	IPC_API int ipc_receive(
		IPC_HANDLE handle,
		void* buffer,
		int size
	);



	/**
	 * @brief  释放IPC资源 
	 * @param handle   IPC连接句柄（ipc_server_create或ipc_client_connect获取的句柄）   
	 */
	IPC_API void ipc_close(
		IPC_HANDLE handle
	);



#ifdef __cplusplus
}
#endif