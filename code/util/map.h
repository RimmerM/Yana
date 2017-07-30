#pragma once

#include "array.h"

static const Size kMinBuckets = 16;

struct MallocAllocator {
    void* alloc(U32 size) {
        return malloc(size);
    }

    void free(void* p) {
        ::free(p);
    }
};

template<class T, class Key = U32, Key emptyKey = Key(-1), class Allocator = MallocAllocator>
struct HashMap : Allocator {
    struct Add {
        T* value;
        bool isExisting;
    };

    HashMap() = default;

    HashMap(Size reservedSize) {
        reserve(reservedSize);
    }

    ~HashMap() {
        clear();
    }

    const T* get(Key key) const {
        auto it = lookup(key);
        if(it && it->key != emptyKey) return &it->data;
        else return nullptr;
    }

    T* get(Key key) {
        auto it = lookup(key);
        if(it && it->key != emptyKey) return &it->data;
        else return nullptr;
    }

    /// Adds the provided item, overwriting any existing one.
    /// Returns true if a previous value was overwritten.
    bool add(Key key, const T& value) {
        auto it = add(key);
        if(it.isExisting) {
            it.value->~T();
        }

        new (it.value) T(value);
        return it.isExisting;
    }

    /// Adds the provided item, overwriting any existing one.
    /// Returns true if a previous value was overwritten.
    bool add(Key key, T&& value) {
        auto it = add(key);
        if(it.isExisting) {
            it.value->~T();
        }

        new (it.value) T(forward<T>(value));
        return it.isExisting;
    }

    /**
     * Gets or adds a map item for the provided hash.
     * Returns either a pointer to a new, uninitialized item, or a pointer to an existing item.
     */
    Add add(Key key) {
        reserve(count + 1);

        // This will always return a valid pointer, since we just increased the size.
        auto existing = lookup(key);
        if(existing->key == emptyKey) {
            // We created a new item.
            count++;
            existing->key = key;
            return {&existing->data, false};
        } else {
            return {&existing->data, true};
        }
    }

    /**
     * Removes the element with the provided key, if any exists.
     * @return True if an element was removed.
     */
    bool remove(Key key) {
        return remove(key, [](auto v) {});
    }

    /**
     * Removes the element with the provided key, if any exists.
     * @param fun This function is called on the element about to be removed, if any.
     * @return True if an element was removed.
     */
    template<class F>
    bool remove(Key key, F&& fun) {
        auto value = lookup(key);
        if(!value || value->key == emptyKey) return false;

        value->data.~T();
        count--;

        auto end = objects + space;
        auto i = value;
        while(1) {
            i++;
            if(i == end) i = objects;

            if(i->key == emptyKey) break;

            auto it = objects + (i->key & (space - 1));
            if((i > value && (it <= value || it > i)) || (i < value && (it <= value && it > i))) {
                copyMem(&i->data, &value->data, sizeof(T));
                value->key = i->key;
                value = i;
            }
        }

        value->key = emptyKey;
        return true;
    }

    void clear() {
        for(auto& v: *this) {
            v.~T();
        }

#ifdef DEBUG
        setMem(objects, sizeof(Object<T>) * space, 0xfe);
#endif
        Allocator::free(objects);
        objects = nullptr;
        count = 0;
        space = 0;
    }

    Size size() const {
        return count;
    }

    Size buckets() const {
        return space;
    }

    T& operator[] (Key key) {
        auto it = add(key);
        if(!it.isExisting) {
            new (it.value) T;
        }
        return *it.value;
    }

    bool reserve(Size count) {
        count *= 2;
        if(count > space) {
            rehash(count);
            return true;
        } else {
            return false;
        }
    }

private:
    template<class U> struct Object {
        Key key;
        U data;
    };

    static Size roundupCount(Size count) {
        count--;
        count |= count >> 1;
        count |= count >> 2;
        count |= count >> 4;
        count |= count >> 8;
        count |= count >> 16;
        count++;
        return count;
    }

    static Object<T>* lookup(Key key, Object<T>* objects, Size space) {
        if(space == 0) return nullptr;

        auto i = key;
        while(1) {
            i &= space - 1;
            auto it = objects + i;
            if(it->key == key || it->key == emptyKey) {
                return it;
            }
            i++;
        }
    }

    Object<T>* lookup(Key key) const {
        return lookup(key, objects, space);
    }

    Object<T>* skip(Object<T>* objects) const {
        auto end = this->objects + space;
        while(objects < end && objects->key == emptyKey) {
            objects++;
        }
        return objects;
    }

    void rehash(Size count) {
        count = roundupCount(count);
        if(count < kMinBuckets) count = kMinBuckets;

        if(count > space) {
            auto objects = (Object<T>*)Allocator::alloc(sizeof(Object<T>) * count);
            for(auto i = 0; i < count; i++) {
                objects[i].key = emptyKey;
            }

            auto end = this->end();
            for(auto i = begin(); i != end; ++i) {
                auto it = lookup(i.key(), objects, count);
                assert(it && it->key == emptyKey);
                it->key = i.key();
                memcpy(&it->data, i.value(), sizeof(T));
            }

#ifdef DEBUG
            memset(this->objects, 0xfe, sizeof(Object<T>) * space);
#endif
            Allocator::free(this->objects);
            this->objects = objects;
            this->space = (U32)count;
        }
    }

public:
    template<class M, class U>
    struct ItT {
        ItT(M& map, Object<U>* p) : map(map), p(p) {}
        U& operator * () {return p->data;}
        U* operator -> () {return &p->data;}
        bool operator == (ItT a) {return p == a.p;}
        bool operator != (ItT a) {return p != a.p;}
        Size operator - (ItT a) const {return p - a.p;}

        Key key() { return p->key; }
        U* value() { return &p->data; }

        ItT operator ++ () {
            p = map.skip(p + 1);
            return *this;
        }

    private:
        M& map;
        Object<U>* p;
    };

    using I = ItT<HashMap<T, Key, emptyKey, Allocator>, T>;
    using CI = ItT<const HashMap<T, Key, emptyKey, Allocator>, const T>;

    I begin() {return {*this, objects ? skip(objects) : nullptr};}
    I end() {return {*this, objects + space};}
    CI begin() const {return {*this, objects ? skip(objects) : nullptr};}
    CI end() const {return {*this, objects + space};}

private:
    Object<T>* objects = nullptr;
    U32 count = 0;
    U32 space = 0;
};

//----------------------------------------------------------------------------------------------------------

template<class T, class K, K e, class A, class F>
void walk(F f, const HashMap<T, K, e, A>& map) {
    for(auto& i : map) {f(i);}
}

template<class T, class K, K e, class A, class F>
void modify(F f, HashMap<T, K, e, A>& map) {
    for(auto& i : map) {f(i);}
}

template<class T, class K, K e, class A, class U, class F>
auto fold(F f, U start, typename HashMap<T, K, e, A>::CI begin, typename HashMap<T, K, e, A>::CI end) {
    if(begin != end) {
        auto next = begin; ++next;
        return fold(f, f(*begin, start), next, end);
    }
    else return start;
}

template<class T, class K, K e, class A, class U, class F>
auto fold(F f, U start, const HashMap<T, K, e, A>& map) {
    return fold(f, start, map.begin(), map.end());
}