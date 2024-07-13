#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
#include <string>
#include <vector>
#include <map>

using namespace std;
using namespace muduo;
//返回单例对象的接口函数
ChatService* ChatService::instance(){
    static ChatService service;
    return &service;
}

//注册消息以及对应的handler回调函数
ChatService::ChatService(){
    _msgHandlerMap.insert({LOGIN_MSG,std::bind(&ChatService::login,this,_1,_2,_3)});
    _msgHandlerMap.insert({REG_MSG,std::bind(&ChatService::reg,this,_1,_2,_3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,std::bind(&ChatService::oneChat,this,_1,_2,_3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG,std::bind(&ChatService::addFriend,this,_1,_2,_3)});
    
    //群聊回调函数
    _msgHandlerMap.insert({CREATE_GROUP_MSG,std::bind(&ChatService::createGroup,this,_1,_2,_3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG,std::bind(&ChatService::addGroup,this,_1,_2,_3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG,std::bind(&ChatService::groupChat,this,_1,_2,_3)});

    if (_redis.connect())
    {
        //设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handldeRedisSubscribMessage,this,_1,_2));
    }
    
}

MsgHandler ChatService::getHandler(int msgid){
    //记录错误日志，msgid没有对应的事件处理回调
    auto it=_msgHandlerMap.find(msgid);
    if(it==_msgHandlerMap.end()){
        //返回一个默认的处理器，空操作
        return [=](const TcpConnectionPtr &conn, json &js,Timestamp time){
            LOG_ERROR <<"msgid:"<<msgid<<" can not find handler!";
        };
    }else{
        return _msgHandlerMap[msgid];
    }
}

//服务器异常，业务重置方法
void ChatService::reset(){
    //把online状态的用户，设置成offline
    _UserModel.resetState();
}


//处理登陆业务 id pwd  
void ChatService::login(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int id=js["id"].get<int>();
    string pwd=js["password"];

    User user=_UserModel.query(id);
    if(user.getId()==id && user.getPwd()==pwd){
        if(user.getState()=="online"){
            //用户已登陆，不允许重复登陆
            json response;
            response["msgid"]=LOGIN_MSG_ACK;
            response["errno"]=2;
            response["errmsg"]="this account is using,input another!";


            conn->send(response.dump());
        }
        else{
            //登陆成功，记录用户连接信息(考虑线程安全问题)
            {
                //只需要在插入操作的作用域加锁，出了此作用域后是对mysql进行操作，无需考虑线程安全(mysql底层自己解决)
                //lock_guard的构造函数是加锁，析构函数会解锁
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({id,conn});
            }

            //id用户登录成功后，向redis订阅channel(id)
            _redis.subscribe(id);

            //更新用户状态信息 state offline=>online
            user.setState("online");
            _UserModel.updateState(user);

            json response;
            response["msgid"]=LOGIN_MSG_ACK;
            response["errno"]=0;
            response["id"]=user.getId();
            response["name"]=user.getName();

            //查询该用户是否有离线消息
            vector<string> vec=_offlineMsgModel.query(id);
            if(!vec.empty()){
                response["offlinemsg"]=vec;
                //读取该用户的离线消息后，把该用户的所有离线消息删除掉
                _offlineMsgModel.remove(id);
            }

            //查询该用户的好友信息并返回
            vector<User> userVec=_friendModel.query(id);
            if(!userVec.empty()){
                vector<string> vec2;
                for(User &user:userVec){
                    json js;
                    js["id"]=user.getId();
                    js["name"]=user.getName();
                    js["state"]=user.getState();

                    vec2.push_back(js.dump());
                }
                response["friend"]=vec2;
            }

            //查询用户的群组信息
            vector<Group> groupVec=_groupModel.queryGroups(id);
            if(!groupVec.empty()){
                vector<string> groupV;
                for(auto &group:groupVec){
                    json grpjson;
                    grpjson["id"]=group.getId();
                    grpjson["groupname"]=group.getName();
                    grpjson["groupdesc"]=group.getDesc();

                    vector<string> userV;
                    for(GroupUser &user:group.getUsers()){
                        json js;
                        js["id"]=user.getId();
                        js["name"] = user.getName();
                        js["state"] = user.getState();
                        js["role"] = user.getRole();

                        userV.push_back(js.dump());
                    }

                    grpjson["users"]=userV;
                    groupV.push_back(grpjson.dump());
                }
                
                response["groups"] = groupV;
            }

            conn->send(response.dump());

        }
    }else{
        //用户不存在，登陆失败
        json response;
        response["msgid"]=LOGIN_MSG_ACK;
        response["errno"]=1;
        response["errmsg"]="name or password is invaild!";

        conn->send(response.dump());
    }
}

//处理注册业务 name password
void ChatService::reg(const TcpConnectionPtr &conn, json &js,Timestamp time){
    string name=js["name"];
    string pwd=js["password"];

    User user;
    user.setName(name);
    user.setPwd(pwd);
    bool state=_UserModel.insert(user);

    if(state){
        //注册成功
        json response;
        response["msgid"]=REG_MSG_ACK;
        response["errno"]=0;
        response["id"]=user.getId();

        conn->send(response.dump());
    }else{
        //注册失败
        json response;
        response["msgid"]=REG_MSG_ACK;
        response["errno"]=1;//0表示没错，1表示有错

        conn->send(response.dump());
    }
}

//处理客户端异常退出
void ChatService::clientCloseException(const TcpConnectionPtr &conn){
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for(auto it=_userConnMap.begin();it!=_userConnMap.end();it++){
            if(it->second==conn){
                //从map表删除用户的链接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    //用户注销，取消redis中的订阅
    _redis.unsubscribe(user.getId());

    //更新用户的状态信息
    user.setState("offline");
    _UserModel.updateState(user);

    //更新用户的状态信息
    if(user.getId()!=-1){
        user.setState("offline");
        _UserModel.updateState(user);
    }
}

//一对一聊天
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int toid=js["to"].get<int>();

    {
        lock_guard<mutex>lock (_connMutex);
        auto it=_userConnMap.find(toid);
        if(it!=_userConnMap.end()){
            //toid在线，转发消息,服务器主动推送消息给toid用户
            it->second->send(js.dump());
            return;
        }
    }

    //查询user是否在线
    User _user=_UserModel.query(toid);
    if(_user.getState()=="online"){
        _redis.publish(toid,js.dump());
        return;
    }
    //toid不在线，存储离线消息
    _offlineMsgModel.insert(toid,js.dump());
}

//添加好友业务 msgid id friendid
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int userid=js["id"].get<int>();
    int friendid=js["friendid"].get<int>();

    //存储好友信息
    _friendModel.insert(userid,friendid);
}

//创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int userid=js["id"].get<int>();
    string name=js["groupname"];
    string desc=js["groupdesc"];

    //存储新创建的群组的消息
    Group group(-1,name,desc);
    if(_groupModel.createGroup(group)){
        //存储群组创建人的信息
        _groupModel.addGroup(userid,group.getId(),"creator");
    }
}

//加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int userid=js["id"].get<int>();
    int groupid=js["groupid"].get<int>();

    _groupModel.addGroup(userid,groupid,"normal");
}

//群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js,Timestamp time){
    int userid=js["id"].get<int>();
    int groupid=js["groupid"].get<int>();

    vector<int> useridVec=_groupModel.queryGroupUsers(userid,groupid);

    lock_guard<mutex> lock(_connMutex);//防止在转发期间connmap表出现线程安全问题
    for(int id:useridVec){
        auto it=_userConnMap.find(id);
        if(it!=_userConnMap.end()){
            //转发群消息
            it->second->send(js.dump());
        }else{
            //查询user是否在线
            User _user=_UserModel.query(id);
            if(_user.getState()=="online"){
                _redis.publish(id,js.dump());
            } else{
                //toid不在线，存储离线群消息
                _offlineMsgModel.insert(id,js.dump());
            }
        }
    }
}

void ChatService::handldeRedisSubscribMessage(int userid,string msg){
    lock_guard<mutex> lock(_connMutex);
    auto it=_userConnMap.find(userid);
    if(it!=_userConnMap.end()){
        it->second->send(msg);
        return;
    }

    //存储离线消息
    _offlineMsgModel.insert(userid,msg);
}
