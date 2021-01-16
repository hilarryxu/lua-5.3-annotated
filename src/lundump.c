/*
** $Id: lundump.c,v 2.44.1.1 2017/04/19 17:20:42 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

//=====================================================================
// 加载 luac 文件
//=====================================================================

#define lundump_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"


#if !defined(luai_verifycode)
#define luai_verifycode(L,b,f)  /* empty */
#endif



//---------------------------------------------------------------------
// 加载状态
//---------------------------------------------------------------------
typedef struct {
  lua_State *L;
  // io 抽象
  ZIO *Z;
  const char *name;
} LoadState;



//---------------------------------------------------------------------
// 错误处理函数
//---------------------------------------------------------------------
static l_noret error(LoadState *S, const char *why) {
  luaO_pushfstring(S->L, "%s: %s precompiled chunk", S->name, why);
  luaD_throw(S->L, LUA_ERRSYNTAX);
}


/*
** All high-level loads go through LoadVector; you can change it to
** adapt to the endianness of the input
*/
#define LoadVector(S,b,n)	LoadBlock(S,b,(n)*sizeof((b)[0]))


//---------------------------------------------------------------------
// 读取数据到缓冲区
//---------------------------------------------------------------------
static void LoadBlock (LoadState *S, void *b, size_t size) {
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, "truncated");
}



//---------------------------------------------------------------------
// 加载数值
//---------------------------------------------------------------------
#define LoadVar(S,x)		LoadVector(S,&x,1)



//---------------------------------------------------------------------
// 读取一个 lu_byte
//---------------------------------------------------------------------
static lu_byte LoadByte (LoadState *S) {
  lu_byte x;
  LoadVar(S, x);
  return x;
}


//---------------------------------------------------------------------
// 读取一个 int
//---------------------------------------------------------------------
static int LoadInt (LoadState *S) {
  int x;
  LoadVar(S, x);
  return x;
}


//---------------------------------------------------------------------
// 读取一个 lua_Number
//---------------------------------------------------------------------
static lua_Number LoadNumber (LoadState *S) {
  lua_Number x;
  LoadVar(S, x);
  return x;
}


//---------------------------------------------------------------------
// 读取一个 lua_Integer
//---------------------------------------------------------------------
static lua_Integer LoadInteger (LoadState *S) {
  lua_Integer x;
  LoadVar(S, x);
  return x;
}


static TString *LoadString (LoadState *S) {
  size_t size = LoadByte(S);
  if (size == 0xFF)
    LoadVar(S, size);
  if (size == 0)
    return NULL;
  else if (--size <= LUAI_MAXSHORTLEN) {  /* short string? */
    char buff[LUAI_MAXSHORTLEN];
    LoadVector(S, buff, size);
    return luaS_newlstr(S->L, buff, size);
  }
  else {  /* long string */
    TString *ts = luaS_createlngstrobj(S->L, size);
    LoadVector(S, getstr(ts), size);  /* load directly in final place */
    return ts;
  }
}


static void LoadCode (LoadState *S, Proto *f) {
  int n = LoadInt(S);
  f->code = luaM_newvector(S->L, n, Instruction);
  f->sizecode = n;
  LoadVector(S, f->code, n);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);


static void LoadConstants (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S);
  f->k = luaM_newvector(S->L, n, TValue);
  f->sizek = n;
  for (i = 0; i < n; i++)
    setnilvalue(&f->k[i]);
  for (i = 0; i < n; i++) {
    TValue *o = &f->k[i];
    int t = LoadByte(S);
    switch (t) {
    case LUA_TNIL:
      setnilvalue(o);
      break;
    case LUA_TBOOLEAN:
      setbvalue(o, LoadByte(S));
      break;
    case LUA_TNUMFLT:
      setfltvalue(o, LoadNumber(S));
      break;
    case LUA_TNUMINT:
      setivalue(o, LoadInteger(S));
      break;
    case LUA_TSHRSTR:
    case LUA_TLNGSTR:
      setsvalue2n(S->L, o, LoadString(S));
      break;
    default:
      lua_assert(0);
    }
  }
}


static void LoadProtos (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S);
  f->p = luaM_newvector(S->L, n, Proto *);
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = NULL;
  for (i = 0; i < n; i++) {
    f->p[i] = luaF_newproto(S->L);
    LoadFunction(S, f->p[i], f->source);
  }
}


static void LoadUpvalues (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);
  f->upvalues = luaM_newvector(S->L, n, Upvaldesc);
  f->sizeupvalues = n;
  for (i = 0; i < n; i++)
    f->upvalues[i].name = NULL;
  for (i = 0; i < n; i++) {
    f->upvalues[i].instack = LoadByte(S);
    f->upvalues[i].idx = LoadByte(S);
  }
}


static void LoadDebug (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);
  f->lineinfo = luaM_newvector(S->L, n, int);
  f->sizelineinfo = n;
  LoadVector(S, f->lineinfo, n);
  n = LoadInt(S);
  f->locvars = luaM_newvector(S->L, n, LocVar);
  f->sizelocvars = n;
  for (i = 0; i < n; i++)
    f->locvars[i].varname = NULL;
  for (i = 0; i < n; i++) {
    f->locvars[i].varname = LoadString(S);
    f->locvars[i].startpc = LoadInt(S);
    f->locvars[i].endpc = LoadInt(S);
  }
  n = LoadInt(S);
  for (i = 0; i < n; i++)
    f->upvalues[i].name = LoadString(S);
}



//---------------------------------------------------------------------
// 读取函数原型
//---------------------------------------------------------------------
static void LoadFunction (LoadState *S, Proto *f, TString *psource) {
  // 读取 source 信息，没有就用父函数的
  f->source = LoadString(S);
  if (f->source == NULL)  /* no source in dump? */
    f->source = psource;  /* reuse parent's source */
  // 起始结束行号
  f->linedefined = LoadInt(S);
  f->lastlinedefined = LoadInt(S);
  // 参数个数
  f->numparams = LoadByte(S);
  // 是否变长参数
  f->is_vararg = LoadByte(S);
  // 寄存器数量
  f->maxstacksize = LoadByte(S);
  // 指令代码
  LoadCode(S, f);
  // 常量
  LoadConstants(S, f);
  // upvalue
  LoadUpvalues(S, f);
  // 子函数原型表
  LoadProtos(S, f);
  // 调试相关信息
  LoadDebug(S, f);
}



//---------------------------------------------------------------------
// 读取并比较字符串
//---------------------------------------------------------------------
static void checkliteral (LoadState *S, const char *s, const char *msg) {
  char buff[sizeof(LUA_SIGNATURE) + sizeof(LUAC_DATA)]; /* larger than both */
  size_t len = strlen(s);
  LoadVector(S, buff, len);
  if (memcmp(s, buff, len) != 0)
    error(S, msg);
}



//---------------------------------------------------------------------
// 读取一个字节并比较
//---------------------------------------------------------------------
static void fchecksize (LoadState *S, size_t size, const char *tname) {
  if (LoadByte(S) != size)
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", tname));
}


#define checksize(S,t)	fchecksize(S,sizeof(t),#t)

static void checkHeader (LoadState *S) {
  // signature
  checkliteral(S, LUA_SIGNATURE + 1, "not a");  /* 1st char already checked */
  // 版本
  if (LoadByte(S) != LUAC_VERSION)
    error(S, "version mismatch in");
  // luac 文件格式
  if (LoadByte(S) != LUAC_FORMAT)
    error(S, "format mismatch in");
  checkliteral(S, LUAC_DATA, "corrupted");
  // int 类型大小
  checksize(S, int);
  // size_t 类型大小
  checksize(S, size_t);
  // 指令占用字节数
  checksize(S, Instruction);
  // lua_Integer 类型大小
  checksize(S, lua_Integer);
  // lua_Number 类型大小
  checksize(S, lua_Number);
  // 0x5678 检测大小端
  if (LoadInteger(S) != LUAC_INT)
    error(S, "endianness mismatch in");
  // 370.5 检查浮点数表示是否一致
  if (LoadNumber(S) != LUAC_NUM)
    error(S, "float format mismatch in");
}


/*
** load precompiled chunk
*/
LClosure *luaU_undump(lua_State *L, ZIO *Z, const char *name) {
  LoadState S;
  LClosure *cl;

  // 解析闭包名称信息
  if (*name == '@' || *name == '=')
    S.name = name + 1;
  else if (*name == LUA_SIGNATURE[0])
    S.name = "binary string";
  else
    S.name = name;

  S.L = L;
  S.Z = Z;

  // 检测文件头
  checkHeader(&S);
  // 读取 upvalue 个数，新建一个 LClosure 结构
  cl = luaF_newLclosure(L, LoadByte(&S));
  setclLvalue(L, L->top, cl);
  luaD_inctop(L);
  cl->p = luaF_newproto(L);
  // 读取 main proto
  LoadFunction(&S, cl->p, NULL);
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luai_verifycode(L, buff, cl->p);
  return cl;
}

