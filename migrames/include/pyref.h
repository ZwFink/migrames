#ifndef PYREF_HH_INCLUDED
#define PYREF_HH_INCLUDED
#define PY_SSIZE_T_CLEAN
#include <Python.h>



template <typename T>
class py_strongref {
    T *obj;
    public:
    py_strongref(T *obj) : obj(obj) {
        Py_INCREF((PyObject*) obj);
    }

    py_strongref() : obj(NULL) {}
    py_strongref(const py_strongref &other) : obj(other.obj) {
        Py_INCREF((PyObject*) obj);
    }

    ~py_strongref() {
        Py_XDECREF((PyObject*) obj);
    }

    PyObject *operator*() {
        return this->borrow();
    }

    PyObject *borrow() const {
        return (PyObject*) obj;
    }

    void operator=(PyObject *new_obj) {
        // always steals
        obj = new_obj;
    }

    static py_strongref steal(T *obj) {
        py_strongref<T> ref;
        ref = obj;
        return ref;
    }
};

template <typename T>
class py_weakref {
    T *obj;
    public:
    py_weakref(T *obj) : obj(obj) {
    }

    py_weakref() : obj(NULL) {}
    py_weakref(const py_weakref &other) : obj(other.obj) {
    }
    py_weakref(const py_strongref<T> &other) : obj(other.borrow()) {
    }

    PyObject *operator*() {
        return this->borrow();
    }

    PyObject *borrow() {
        return (PyObject*) obj;
    }

    void operator=(PyObject *new_obj) {
        // always steals
        obj = new_obj;
    }
};

using pyobject_strongref = py_strongref<PyObject>;
using pyobject_weakref = py_weakref<PyObject>;
#endif