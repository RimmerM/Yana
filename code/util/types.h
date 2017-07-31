#pragma once

// Windows platforms
#ifdef _WIN32
#define __WINDOWS__ 1

//Define this to disable Windows XP support.
//Doing so allows us to use some OS features only available in Vista and above.
#define NO_XP_SUPPORT 1

//Define this to completely disable the use of the standard library,
//allowing compilation with -nostdlib.
//#define USE_NO_STDLIB 1

#endif // _WIN32

// Darwin platforms
#ifdef __APPLE__

#include "TargetConditionals.h"

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#define __IOS__ 1
#else
#define __OSX__ 1
#endif

#endif // __APPLE__

// Linux platforms
#ifdef __linux__
#define __LINUX__ 1
#endif

#if defined (__APPLE__) || defined (__LINUX__)
#define __POSIX__ 1
#endif

// Check if we are on ARM
#if defined(__arm__) || defined(_M_ARM)
#define __ARM__ 1
#endif // __arm__ || _M_ARM

#ifdef __aarch64__
#define __ARM__ 1
#define __X64__ 1
#endif

// Check if we are on x86.
#if defined(__i386__) || defined(_M_IX86)
#define __X86__ 1
#endif

// Check if we are on x86-64.
#if defined(__x86_64__) || defined(_M_X64)
#define __X86__ 1
#define __X64__ 1
#endif

/*
 * Base types.
 */

using U8 = unsigned char;
using I8 = char;
using U16 = unsigned short;
using I16 = short;
using U32 = unsigned int;
using I32 = int;
using F32 = float;
using F64 = double;
using Float = float;

using Byte = unsigned char;
using Bool = bool;

#ifdef __X64__
#ifdef __WINDOWS__
using Size = unsigned long long;
using Int = long long;
#else //__WINDOWS__
using Size = unsigned long;
using Int = long;
#endif //__WINDOWS__
using I64 = Int;
using U64 = Size;
#else //__X64__
using Size = unsigned int;
using Int = int;
using I64 = long long;
using U64 = unsigned long long;
#endif //__X64__

using Char = char;
using Nullptr = decltype(nullptr);
using Handle = void*;

#ifdef __WINDOWS__
using WChar = wchar_t;
using WChar32 = I32;
#endif

#ifdef __POSIX__
using WChar = I16;
using WChar32 = wchar_t;
#endif

#ifdef _MSC_VER
#define forceinline __forceinline
#define noinline __declspec(noinline)
#define THREAD_LOCAL __declspec(thread)
#else
#define forceinline inline __attribute__ ((always_inline))
#define noinline __attribute__ ((noinline))
#define THREAD_LOCAL __thread
#endif

/*
 * Template stuff.
 */

template<class T, T Val> struct integralConstant {
    static constexpr const T value = Val;
    using type = integralConstant;
    constexpr operator T() const {return Val;}
};

using trueConstant = integralConstant<bool, true>;
using falseConstant = integralConstant<bool, false>;

template<class T> struct isLReference : falseConstant {};
template<class T> struct isLReference<T&> : trueConstant {};

template<class T> struct removeReference {typedef T type;};
template<class T> struct removeReference<T&> {typedef T type;};
template<class T> struct removeReference<T&&> {typedef T type;};

template<class T> struct removeAllExtents {typedef T type;};
template<class T> struct removeAllExtents<T[]> {typedef typename removeAllExtents<T>::type type;};
template<class T, Size N> struct removeAllExtents<T[N]> {typedef typename removeAllExtents<T>::type type;};

template<class T> constexpr typename removeReference<T>::type&& move(T&& t) {return (typename removeReference<T>::type&&)t;}

template<class T> constexpr T&& forward(typename removeReference<T>::type& t) {return (T&&)t;}
template<class T> constexpr T&& forward(typename removeReference<T>::type&& t) {
    static_assert(!isLReference<T>::value, "Cannot forward an rvalue as an lvalue.");
    return (T&&)t;
}

template<class T> void swap(T& x, T& y) {
    T z(move(x));
    x = move(y);
    y = move(z);
}

template<class T> struct Uninitialized {
    template<class... P> void init(P&&... p) {
        new (data) T(forward<P>(p)...);
    }

    T* operator -> () {return (T*)data;}
    operator T& () {return *(T*)data;}
    T* operator & () {return (T*)data;}
    T& operator * () {return *(T*)data;}

    const T* operator -> () const {return (T*)data;}
    operator const T& () const {return *(T*)data;}
    const T* operator & () const {return (T*)data;}
    const T& operator * () const {return *(T*)data;}

private:
    Size data[(sizeof(T) + sizeof(Size) - 1) / sizeof(Size)];
};

#if !defined(_NEW) && !defined(_LIBCPP_NEW)

inline void* operator new (Size, void* data) throw() {return data;}
inline void* operator new[] (Size, void *data) throw() {return data;}

inline void operator delete (void*, void*) throw() {}
inline void operator delete[] (void*, void*) throw() {}

#endif // !_NEW

extern "C" void* memset(void* p, int v, Size count);
extern "C" void* memcpy(void* dst, const void* src, Size count);
extern "C" void* memmove(void* dst, const void* src, Size count);
