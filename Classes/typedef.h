//
//  typedef.h
//  wanaka-framework
//
//  Created by TianBaideng on 15/1/15.
//  Copyright (c) 2015年 wanaka. All rights reserved.
//

#ifndef wanaka_framework_typedef_h
#define wanaka_framework_typedef_h

#ifdef __GNUC__
typedef long long int64;
typedef int int32;
typedef short int16;
typedef signed char int8;
typedef unsigned long long uint64;
typedef unsigned int uint32;
typedef unsigned short uint16;
typedef unsigned char uint8;
typedef unsigned int DWORD;
typedef unsigned char byte;
#else
typedef signed __int64 int64;
typedef signed __int32 int32;
typedef signed __int16 int16;
typedef signed __int8 int8;
typedef unsigned __int64 uint64;
typedef unsigned __int32 uint32;
typedef unsigned __int16 uint16;
typedef unsigned __int8 uint8;
#endif

#endif
