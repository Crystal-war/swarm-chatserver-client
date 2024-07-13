#ifndef GROUPUSER_H
#define GROUPUSER_H

#include "user.hpp"

//群组用户，需要增加一个用户在群组中的角色，其他信息继承自User类
class GroupUser:public User{
public:
    void setRole(string role){this->role=role;}
    string getRole(){return this->role;}

private:
    string role;
};

#endif