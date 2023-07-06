#include "Python.h"

/*
This implements a k-way merge algorithm that acts as a drop-in
replacement for heapq.merge in the standard library. This is
algorithmically superior, as it does fewer comparisons on average
and has no need to store a tuple for each item, nor to compare the
index of which iterator a given item came from.

Written by Dennis Sweeney with ideas from Raymond Hettinger.
See https://bugs.python.org/issue38938.


Algorithm for multimerge.merge:
===========================

- Maintain a binary tree of merge_node objects
- A node N is a leaf iff N->leaf == N.

- For each leaf node:

    - leaf->right will be an iterator
    - leaf->left will be the most recent item produced by leaf->right
    - leaf->leaf will be leaf itself.
    - leaf->key will be the keyfunc(leaf->left).

- For each non-leaf node:

    - node->left and node->right will be merge_nodes
    - node->left->parent == node->right->parent == node
    - node->leaf will be one of node's descendant leaves.
    - node->key will be node->leaf->key.
    - if "winner" is the higher priority of node->left and node->right
      based on node->left->key and node->right->key, then
      node->leaf == winner->leaf and node->key == winner->key

- At a normal merge_next call:

    - Try to get the next value for root->leaf, which holds the
      iterator that produced the previously yielded value.
    - If the iterator is exhausted, delete that leaf and promote its
      sibling to where their common parent is in the tree.
    - Everywhere that the previously yielded champion had won, namely,
      at each of the relevant leaf's ancestors, re-evaluate the winner
      among that ancestor's two children, and copy the winner's leaf
      and key references into that ancestor.

- Reference counting:

    - To prevent cycles, each node's reference to its parent will be
      borrowed.
    - To prevent extra INCREF/DECREFs in the innermost DO_GAMES loop,
      only leaves will have strong references to their keys.
    - All left and right attributes will be strong references because
      leaves hold the sole strong references to their items and
      iterators, and other nodes hold the sole strong references to
      their children.
    - The merge object holds the sole strong reference to the root.
*/

typedef struct merge_state {
    PyObject *merge_type;
} merge_state;

static merge_state *
get_merge_state(PyObject *module)
{
    return (merge_state *)PyModule_GetState(module);
}

static int
mergemodule_traverse(PyObject *module, visitproc visit, void *arg)
{
    merge_state *state = get_merge_state(module);
    Py_VISIT(state->merge_type);
    return 0;
}

static int
mergemodule_clear(PyObject *module)
{
    merge_state *state = get_merge_state(module);
    Py_CLEAR(state->merge_type);
    return 0;
}

static void
mergemodule_free(PyObject *module)
{
    merge_state *state = get_merge_state(module);
    Py_CLEAR(state->merge_type);
}

/* merge node object ********************************************************/

struct merge_node;

typedef struct merge_node {
    PyObject_HEAD
    PyObject *key;             /* strong if is_leaf(node) else borrowed */
    struct merge_node *leaf;   /* borrowed */
    struct merge_node *parent; /* borrowed */
    PyObject *left;            /* strong */
    PyObject *right;           /* strong */
} merge_node;

static PyTypeObject merge_node_type;

static inline int
is_leaf(merge_node *node)
{
    return node->leaf == node;
}

static inline PyObject *
leaf_pop_item(merge_node *node)
{
    assert(is_leaf(node));
    PyObject *res = node->left;
    node->left = NULL;
    Py_CLEAR(node->key);
    return res;
}

static inline PyObject *
leaf_iterator(merge_node *node)
{
    assert(is_leaf(node));
    assert(PyIter_Check(node->right));
    return node->right;
}

static inline merge_node *
left_child(merge_node *node)
{
    assert(!is_leaf(node));
    assert(Py_TYPE(node->right) == &merge_node_type);
    return (merge_node *)node->left;
}

static inline merge_node *
right_child(merge_node *node)
{
    assert(!is_leaf(node));
    assert(Py_TYPE(node->right) == &merge_node_type);
    return (merge_node *)node->right;
}

static int
merge_node_clear(merge_node *node)
{
    if (is_leaf(node)) {
        Py_CLEAR(node->key);
    }
    Py_CLEAR(node->left);
    Py_CLEAR(node->right);
    return 0;
}

static int
merge_node_traverse(merge_node *node, visitproc visit, void *arg)
{
    if (is_leaf(node)) {
        Py_VISIT(node->key);
    }
    Py_VISIT(node->left);
    Py_VISIT(node->right);
    return 0;
}

static void
merge_node_dealloc(merge_node *node)
{
    PyObject_GC_UnTrack(node);
    if (is_leaf(node)) {
        Py_XDECREF(node->key);
    }
    Py_XDECREF(node->left);
    Py_XDECREF(node->right);
    Py_TYPE(node)->tp_free(node);
}

static PyTypeObject merge_node_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "multimerge.merge_node",
    .tp_basicsize = sizeof(merge_node),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor)merge_node_dealloc,
    .tp_clear = (inquiry)merge_node_clear,
    .tp_traverse = (traverseproc)merge_node_traverse,
    .tp_free = PyObject_GC_Del,
};

/* merge object *************************************************************/

typedef struct {
    PyObject_HEAD
    merge_node *root;
    PyObject *iterables;
    PyObject *keyfunc;
    char reverse;
    char state;
} mergeobject;

static PyTypeObject merge_type;

static int
construct_leaf(PyObject *iterable, PyObject *keyfunc, merge_node **node)
{
    PyObject *it = NULL, *item = NULL, *key = NULL;

    assert(iterable != NULL);
    it = PyObject_GetIter(iterable);
    if (it == NULL) {
        goto error;
    }

    item = PyIter_Next(it);
    if (item == NULL) {
        if (PyErr_Occurred()) {
            goto error;
        }
        Py_DECREF(it);
        return 0;
    }

    if (keyfunc == NULL) {
        Py_INCREF(item);
        key = item;
    }
    else {
        key = PyObject_CallOneArg(keyfunc, item);
        if (key == NULL) {
            goto error;
        }
    }

    *node = PyObject_GC_New(merge_node, &merge_node_type);
    if (*node == NULL) {
        goto error;
    }

    (*node)->key = key;
    (*node)->left = item;
    (*node)->right = it;
    (*node)->parent = NULL;
    (*node)->leaf = (*node);
    return 1;

error:
    Py_XDECREF(it);
    Py_XDECREF(item);
    Py_XDECREF(key);
    return -1;
}

static merge_node *
construct_parent(merge_node *left, merge_node *right, int reverse)
{
    assert(left != NULL);
    assert(right != NULL);

    int cmp;
    if (reverse) {
        cmp = PyObject_RichCompareBool(left->key, right->key, Py_LT);
    }
    else {
        cmp = PyObject_RichCompareBool(right->key, left->key, Py_LT);
    }
    if (cmp < 0) {
        return NULL;
    }
    merge_node *winner = cmp ? right : left;
    merge_node *parent = PyObject_GC_New(merge_node, &merge_node_type);
    if (parent == NULL) {
        return NULL;
    }
    parent->key = winner->key;
    parent->leaf = winner->leaf;
    parent->left = (PyObject *)left;
    parent->right = (PyObject *)right;
    parent->parent = NULL;
    Py_INCREF(left);
    Py_INCREF(right);
    left->parent = right->parent = parent;
    return parent;
}

static int
build_tree(mergeobject *mo)
{
    assert(mo->state == 0);
    assert(PyTuple_CheckExact(mo->iterables));

    Py_ssize_t n0 = PyTuple_GET_SIZE(mo->iterables);
    PyObject *new_nodes = NULL;
    PyObject *nodes = PyList_New(0);
    if (nodes == NULL) {
        return -1;
    }
    
    /* first put each nonempty iterator into a leaf. */
    for (Py_ssize_t i=0; i < n0; i++) {
        PyObject *it = PyTuple_GET_ITEM(mo->iterables, i);
        merge_node *leaf;
        int result = construct_leaf(it, mo->keyfunc, &leaf);
        if (result < 0) {
            goto error;
        }
        if (result) {
            if (PyList_Append(nodes, (PyObject *)leaf) < 0) {
                Py_DECREF(leaf);
                goto error;
            }
            Py_DECREF(leaf);
        }
    }
    Py_CLEAR(mo->iterables);

    n0 = PyList_GET_SIZE(nodes);
    if (n0 == 0) {
        /* All iterators were empty. */
        goto error;
    }
    if (n0 == 1) {
        /* Only one node, so don't compute keys. */
        Py_CLEAR(mo->keyfunc);
    }

    /* Now repeatedly unite pairs of adjacent nodes by adding a common
       parent. Stop once we have one united binary tree. */
    while (n0 > 1) {
        Py_ssize_t n1 = (n0 + 1) / 2;
        new_nodes = PyList_New(n1);
        if (new_nodes == NULL) {
            goto error;
        }
        Py_ssize_t j = 0;
        if (n0 & 1) {
            PyObject *first = PyList_GET_ITEM(nodes, 0);
            Py_INCREF(first);
            PyList_SET_ITEM(new_nodes, j, first);
            j++;
        }
        for (Py_ssize_t i = n0 & 1; i < n0 - 1; (i += 2), (j++)) {
            merge_node *left = (merge_node *)PyList_GET_ITEM(nodes, i);
            merge_node *right = (merge_node *)PyList_GET_ITEM(nodes, i + 1);
            merge_node *parent = construct_parent(left, right, mo->reverse);
            if (parent == NULL) {
                goto error;
            }
            PyList_SET_ITEM(new_nodes, j, (PyObject *)parent);
        }
        assert(j == n1);
        Py_SETREF(nodes, new_nodes);
        n0 = n1;
    }

    mo->root = (merge_node *)PyList_GET_ITEM(nodes, 0);
    Py_INCREF(mo->root);
    Py_DECREF(nodes);
    return 0;

error:
    Py_CLEAR(mo->iterables);
    Py_XDECREF(new_nodes);
    Py_XDECREF(nodes);
    return -1;
}

static int
refill_leaf(merge_node *leaf, PyObject *keyfunc)
{
    PyObject *it = leaf_iterator(leaf);
    PyObject *item = PyIter_Next(it);
    if (item == NULL) {
        if (PyErr_Occurred()) {
            return -1;
        }
        return 0;
    }
    PyObject *key;
    if (keyfunc == NULL) {
        key = item;
        Py_INCREF(key);
    }
    else {
        key = PyObject_CallOneArg(keyfunc, item);
        if (key == NULL) {
            Py_DECREF(item);
            return -1;
        }
    }
    assert(leaf->left == NULL);
    leaf->left = item;
    assert(leaf->key == NULL);
    leaf->key = key;
    return 1;
}

static merge_node *
promote_sibling_of(merge_node *leaf)
{
    merge_node *parent = leaf->parent;
    if (parent == NULL) {
        /* End the iteration now. */
        return NULL;
    }
    merge_node *left = left_child(parent);
    merge_node *right = right_child(parent);
    merge_node *sibling = (leaf == left) ? right : left;

    parent->key = sibling->key;
    parent->leaf = sibling->leaf;
    parent->left = sibling->left;
    parent->right = sibling->right;

    if (is_leaf(sibling)) {
        /* sibling was a leaf, so parent is now a leaf. */
        parent->leaf = parent;
    }
    else {
        left_child(sibling)->parent = parent;
        right_child(sibling)->parent = parent;
    }

    /* Clear these so we don't decref them; 
       they were just moved into the parent. */
    sibling->left = NULL;
    sibling->right = NULL;
    sibling->key = NULL;
    
    Py_DECREF(leaf);
    Py_DECREF(sibling);

    assert(is_leaf(parent->leaf));
    return parent;
}

static int
replay_games(mergeobject *mo)
{
    assert(mo->state == 1);
    merge_node *root = mo->root;
    merge_node *node = root->leaf;
    
    switch (refill_leaf(node, mo->keyfunc)) {
    case -1:
        /* error */
        return -1;
    case 0:
        /* iterator empty */
        node = promote_sibling_of(node);
        if (node == NULL) {
            return -1;
        }
        if (is_leaf(root)) {
            /* Only one iterator is left, so use values as keys. */
            Py_CLEAR(mo->keyfunc);
        }
        break;
    case 1:
        /* got a value */
        break;
    }

    #define DO_GAMES(OP1, OP2) do {                              \
        while (node != root) {                                   \
            node = node->parent;                                 \
            merge_node *left = left_child(node);                 \
            merge_node *right = right_child(node);               \
            int cmp = PyObject_RichCompareBool(OP1, OP2, Py_LT); \
            if (cmp < 0) {                                       \
                return -1;                                       \
            }                                                    \
            merge_node *winner = cmp ? right : left;             \
            node->key = winner->key;                             \
            node->leaf = winner->leaf;                           \
        }                                                        \
    } while (0)

    if (mo->reverse) {
        /* winner = right if left < right else left */
        DO_GAMES(left->key, right->key);
    }
    else {
        /* winner = right if right < left else left */
        DO_GAMES(right->key, left->key);
    }

    #undef DO_GAMES
    return 0;
}

static PyObject *
merge_next(mergeobject *mo)
{
    switch (mo->state) {
    case 0:
        if (build_tree(mo) < 0) {
            mo->state = 2;
            return NULL;
        }
        mo->state = 1;
        break;
    case 1:
        if (replay_games(mo) < 0) {
            mo->state = 2;
            return NULL;
        }
        break;
    case 2:
        return NULL;
    }
    return leaf_pop_item(mo->root->leaf);
}

static PyObject *
merge_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *key = NULL;
    int reverse = 0;

    if (kwds != NULL) {
        char *kwlist[] = {"key", "reverse", NULL};
        PyObject *tmpargs = PyTuple_New(0);
        if (tmpargs == NULL) {
            return NULL;
        }
        if (!PyArg_ParseTupleAndKeywords(tmpargs, kwds, "|Op:merge",
                                         kwlist, &key, &reverse)) {
            Py_DECREF(tmpargs);
            return NULL;
        }
        Py_DECREF(tmpargs);
    }

    int state;

    assert(PyTuple_CheckExact(args));
    if (PyTuple_GET_SIZE(args) == 0) {
        state = 2;
        args = NULL;
        key = NULL;
    }
    else {
        state = 0;
        if (key == Py_None) {
            key = NULL;
        }
    }

    mergeobject *mo = (mergeobject *)type->tp_alloc(type, 0);
    if (mo) {
        Py_XINCREF(args);
        Py_XINCREF(key);
        mo->root = NULL;
        mo->iterables = args;
        mo->keyfunc = key;
        mo->reverse = reverse;
        mo->state = state;
        return (PyObject *)mo;
    }
    return NULL;
}

static int
merge_clear(mergeobject *mo)
{
    Py_CLEAR(mo->root);
    Py_CLEAR(mo->iterables);
    Py_CLEAR(mo->keyfunc);
    return 0;
}

static void
merge_dealloc(mergeobject *mo)
{
    PyObject_GC_UnTrack(mo);
    merge_clear(mo);
    Py_TYPE(mo)->tp_free(mo);
}

static int
merge_traverse(mergeobject *mo, visitproc visit, void *arg)
{
    Py_VISIT(mo->root);
    Py_VISIT(mo->iterables);
    Py_VISIT(mo->keyfunc);
    return 0;
}

PyDoc_STRVAR(merge_doc,
"merge(*iterables, key=None, reverse=False) --> merge object\n\
\n\
Merge multiple sorted inputs into a single sorted output.\n\
\n\
Similar to sorted(itertools.chain(*iterables)) but returns a generator,\n\
does not pull the data into memory all at once, and assumes that each of\n\
the input streams is already sorted (smallest to largest).\n\
\n\
>>> list(merge([1,3,5,7], [0,2,4,8], [5,10,15,20], [], [25]))\n\
[0, 1, 2, 3, 4, 5, 5, 7, 8, 10, 15, 20, 25]\n\
\n\
If *key* is not None, applies a key function to each element to determine\n\
its sort order.\n\
\n\
>>> list(merge(['dog', 'horse'], ['cat', 'fish', 'kangaroo'], key=len))\n\
['dog', 'cat', 'fish', 'horse', 'kangaroo']");

static PyType_Slot merge_type_slots[] = {
    {Py_tp_dealloc, merge_dealloc},
    {Py_tp_doc, (void *)merge_doc},
    {Py_tp_traverse, merge_traverse},
    {Py_tp_clear, merge_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, merge_next},
    {Py_tp_new, merge_new},
    {0, NULL},
};

static PyType_Spec merge_type_spec = {
    .name = "multimerge.merge",
    .basicsize = sizeof(mergeobject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = merge_type_slots,
};

static int
multimerge_exec(PyObject *module)
{
    merge_state *state = get_merge_state(module);
    state->merge_type = PyType_FromModuleAndSpec(module,
                                                 &merge_type_spec, NULL);
    if (state->merge_type == NULL) {
        return -1;
    }
    return PyModule_AddType(module, state->merge_type);
}

static struct PyModuleDef_Slot multimerge_slots[] = {
    {Py_mod_exec, multimerge_exec},
    {0, NULL}
};

static struct PyModuleDef multimergemodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "multimerge",
    .m_size = sizeof(merge_state),
    .m_doc = "implements a k-way merge algorithm as a drop-in\n\
replacement for heapq.merge in the Python standard library",
    .m_slots = multimerge_slots,
    .m_traverse = mergemodule_traverse,
    .m_clear = mergemodule_clear,
    .m_free = mergemodule_free,
};

PyMODINIT_FUNC
PyInit_multimerge(void)
{
    return PyModuleDef_Init(&multimergemodule);
}
