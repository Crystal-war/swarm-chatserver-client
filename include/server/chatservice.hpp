#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include<muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include "json.hpp"
#include "UserModel.hpp"
#include <mutex>
#include "OfflineMessageModel.hpp"
#include "friendmodel.hpp"
#include "groupmodel.hpp"
#include "redis.hpp"

using namespace std;
using namespace muduo;
using namespace muduo::net;
using json=nlohmann::json;

//表示处理消息的事件回调方法类型
using MsgHandler=std::function<void(const TcpConnectionPtr &conn, json &js,Timestamp time)>;

//聊天服务器类
class ChatService{
public:
    //获取单例对象的接口函数
    static ChatService* instance();
    //处理登陆业务
    void login(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //处理注册业务
    void reg(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //一对一聊天业务
    void oneChat(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //添加好友业务
    void addFriend(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //服务器异常，业务重置方法
    void reset();

    //创建群组业务
    void createGroup(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //加入群聊
    void addGroup(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //群组聊天业务
    void groupChat(const TcpConnectionPtr &conn, json &js,Timestamp time);

    //获取消息对应的处理器
    MsgHandler getHandler(int msgid);

    void clientCloseException(const TcpConnectionPtr &conn);

    void handldeRedisSubscribMessage(int id,string msg);
private:
    //单例模式，私有构造函数
    ChatService();

    //存储消息id和其对应的业务处理方法
    unordered_map<int,MsgHandler> _msgHandlerMap;

    //存储在线用户的通信连接(注意线程安全问题)
    unordered_map<int,TcpConnectionPtr> _userConnMap;

    //定义互斥锁保证_userConnMap的线程安全
    mutex _connMutex;

    //数据操作类对象
    UserModel _UserModel;
    OfflineMessageModel _offlineMsgModel;
    FriendModel _friendModel;
    GroupModel _groupModel;

    Redis _redis;
    
};

#endif