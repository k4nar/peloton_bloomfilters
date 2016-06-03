#include<Python.h>
#include<fcntl.h>
#include<math.h>
#include<stddef.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/file.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<unistd.h>

// A reduced complexity, sizeof(uint64_t) only implementation of XXHASH

#define PRIME_1 11400714785074694791ULL
#define PRIME_2 14029467366897019727ULL
#define PRIME_3  1609587929392839161ULL
#define PRIME_4  9650029242287828579ULL
#define PRIME_5  2870177450012600261ULL

#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0
#endif

#ifdef __GNUC__
#define __atomic_or_fetch(X, Y, Z) __sync_or_and_fetch(X, Y)
#define __atomic_fetch_sub(X, Y, Z) __sync_fetch_and_sub(X, Y)
#endif


typedef struct {
  int fd;
  uint64_t capacity;
  double error_rate;
  uint64_t length;
  int probes;
  void *mmap;
  size_t mmap_size;
  uint64_t *bits;
  uint64_t *counter;
  uint64_t local_counter;
  int invert;
} bloomfilter_t;



typedef struct _shared_memory_bloomfilter_object SharedMemoryBloomfilterObject;
struct _shared_memory_bloomfilter_object {
  PyObject HEAD;
  bloomfilter_t *bf;
  PyObject *weakreflist;
};

static inline uint64_t rotl(uint64_t x, uint64_t r) {
  return ((x >> (64 - r)) | (x << r));
}

inline uint64_t xxh64(uint64_t k1) {
  uint64_t h64;
  h64  = PRIME_5 + 8;

  k1 *= PRIME_2;
  k1 = rotl(k1, 31);
  k1 *= PRIME_1;
  h64 ^= k1;
  h64 = rotl(h64, 27) * PRIME_1 + PRIME_4;
  h64 ^= h64 >> 33;
  h64 *= PRIME_2;
  h64 ^= h64 >> 29;
  h64 *= PRIME_3;
  h64 ^= h64 >> 32;
  return h64;
}


inline int bloomfilter_probes(double error_rate) {
  if ((error_rate <= 0) || (error_rate >= 1))
    return -1;
  return (int)(ceil(log(1 / error_rate) / log(2)));
}


size_t bloomfilter_size(uint64_t capacity, double error_rate) {
  uint64_t bits = ceil(2 * capacity * fabs(log(error_rate))) / (log(2) * log(2));
  if (bits % 64)
    bits += 64 - bits % 64;
  return bits;
}

const char HEADER[] = "SharedMemory BloomFilter";


static bloomfilter_t *create_bloomfilter(int fd, uint64_t capacity, double error_rate) {
  bloomfilter_t *bloomfilter;
  char magicbuffer[25];
  uint64_t i;
  uint64_t zero=0;
  struct stat stats;
  if (-1 == bloomfilter_probes(error_rate))
    return NULL;
  if (!(bloomfilter = malloc(sizeof(bloomfilter_t))))
    return NULL;
  flock(fd, LOCK_EX);

  if (fstat(fd, &stats)) 
    goto error;
  if (stats.st_size == 0) {
    bloomfilter->capacity = capacity;
    bloomfilter->probes = bloomfilter_probes(error_rate);
    bloomfilter->length = (bloomfilter_size(capacity, error_rate) + 63) / 64;
    write(fd, HEADER, 24);
    write(fd, &capacity, sizeof(uint64_t));
    write(fd, &error_rate, sizeof(uint64_t));
    write(fd, &capacity, sizeof(uint64_t));
    for(i=0; i< bloomfilter->length; ++i)
      write(fd, &zero, sizeof(uint64_t));
  } else {
    lseek(fd, 0, 0);
    read(fd, magicbuffer, 24);
    if (strncmp(magicbuffer, HEADER, 24)) 
      goto error;

    if (read(fd, &bloomfilter->capacity, sizeof(uint64_t)) < sizeof(uint64_t)) 
      goto error;

    if (read(fd, &bloomfilter->error_rate, sizeof(double)) < sizeof(double)) 
      goto error;

    bloomfilter->probes = bloomfilter_probes(bloomfilter->error_rate);
    bloomfilter->length = (bloomfilter_size(bloomfilter->capacity, bloomfilter->error_rate) + 63) / 64;
  }
  flock(fd, LOCK_UN);
  bloomfilter->mmap_size = 24 + sizeof(double) + sizeof(uint64_t)*3 + bloomfilter->length * sizeof(uint64_t);
  bloomfilter->mmap = mmap(NULL,
                           bloomfilter->mmap_size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_HASSEMAPHORE,
                           fd,
                           0);
  if (!bloomfilter->mmap) 
    goto error;

  madvise(bloomfilter->mmap, bloomfilter->mmap_size, MADV_RANDOM);
  bloomfilter->counter = bloomfilter->mmap + 24 + sizeof(double) + sizeof(uint64_t);
  bloomfilter->bits = bloomfilter->counter + sizeof(uint64_t);
  return bloomfilter;

 error:
  flock(fd, LOCK_UN);
  if (bloomfilter) free(bloomfilter);
  return NULL;

}


static void shared_memory_bloomfilter_destroy(bloomfilter_t *bloomfilter) {
  if (bloomfilter->mmap && bloomfilter->mmap_size) {
    munmap(bloomfilter->mmap, bloomfilter->mmap_size);
  } else {
    free(bloomfilter->bits);
  }
  if (bloomfilter->fd)
    free(bloomfilter->fd);
  free(bloomfilter);
}

static PyObject *
shared_memory_bloomfilter_clear(SharedMemoryBloomfilterObject *smbo, PyObject *_) {
  bloomfilter_t *bf = smbo->bf;
  size_t length = bf->length;
  size_t i;
  uint64_t *data = __builtin_assume_aligned(bf->bits, 16);
  for(i=0; i<length; ++i)
    data[i] = 0;
  *bf->counter = bf->capacity;
  Py_RETURN_NONE;
}


static PyObject *
shared_memory_bloomfilter_add(SharedMemoryBloomfilterObject *smbo, PyObject *item) {
  bloomfilter_t *bloomfilter = smbo->bf;
  int probes = bloomfilter->probes;
  size_t length = bloomfilter->length;
  uint64_t hash = PyObject_Hash(item);
  if (hash == (uint64_t)(-1))
    return NULL;

  uint64_t count=(__atomic_fetch_sub(bloomfilter->counter, (uint64_t)1, 0));
  uint64_t cleared = !count;
  if (cleared || count > bloomfilter->capacity) {
    Py_DECREF(shared_memory_bloomfilter_clear(smbo, NULL));
  }
  uint64_t *data = __builtin_assume_aligned(smbo->bf->bits, 16);
  

  while (probes--) {
    __atomic_or_fetch(data + (hash >> 6) % length, 1<<(hash & 0x3f), 1);
    hash = xxh64(hash);
  }
  return PyBool_FromLong(cleared);
}

PyObject *
shared_memory_bloomfilter_population(SharedMemoryBloomfilterObject *smbo, PyObject *_) {
  size_t length = smbo->bf->length;
  size_t i;
  uint64_t *data = __builtin_assume_aligned(smbo->bf->bits, 16);
  uint64_t population = 0;
  for(i=0; i<length; ++i)
    population += __builtin_popcountll(data[i]);
  return PyInt_FromLong(population);
}

static Py_ssize_t
SharedMemoryBloomFilterObject_len(SharedMemoryBloomfilterObject* smbo)
{
    return smbo->bf->capacity - *smbo->bf->counter;
}

int 
SharedMemoryBloomFilterObject_contains(SharedMemoryBloomfilterObject* smbo, PyObject *item)
{
  bloomfilter_t *bloomfilter = smbo->bf;
  uint64_t *data = __builtin_assume_aligned(bloomfilter->bits, 16);
  int probes = bloomfilter->probes;
  size_t length = bloomfilter->length;
  uint64_t hash = PyObject_Hash(item);
  if (hash == (uint64_t)(-1)) {
    return -1;
  }
    
  while (probes--) {
    if (!(1<<(hash & 0x3f) & *(data + (hash >> 6) % length)))
      return 0;
    hash = xxh64(hash);
  }
  return 1;
}


static PySequenceMethods SharedMemoryBloomfilterObject_sequence_methods = {
  SharedMemoryBloomFilterObject_len, /* sq_length */
  0,				/* sq_concat */
  0,				/* sq_repeat */
  0,				/* sq_item */
  0,				/* sq_slice */
  0,				/* sq_ass_item */
  0,				/* sq_ass_slice */
  (objobjproc)SharedMemoryBloomFilterObject_contains,	/* sq_contains */
};


static PyMethodDef shared_memory_bloomfilter_methods[] = {
  {"add", (PyCFunction)shared_memory_bloomfilter_add, METH_O, NULL},
  {"clear", (PyCFunction)shared_memory_bloomfilter_clear, METH_O, NULL},
  {"population", (PyCFunction)shared_memory_bloomfilter_population, METH_NOARGS, NULL},
  {NULL, NULL}
};

static void shared_memory_bloomfilter_type_dealloc(SharedMemoryBloomfilterObject *smbo) {
    Py_TRASHCAN_SAFE_BEGIN(smbo);
  if (smbo->weakreflist != NULL)
    PyObject_ClearWeakRefs((PyObject *)smbo);
  shared_memory_bloomfilter_destroy(smbo->bf);

  Py_TRASHCAN_SAFE_END(smbo);
}

PyObject *
make_new_shared_memory_bloomfilter(PyTypeObject *type, int fd, uint64_t capacity, double error_rate);


static int 
shared_memory_bloomfilter_init(SharedMemoryBloomfilterObject *self, PyObject *args, PyObject *kwargs) {
  return 0;
}

static PyObject *
shared_memory_bloomfilter_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {

  int fd = 0;
  char *path = NULL;
  PyObject *source = NULL;
  uint64_t capacity = 1000;
  double error_rate = 1.0 / 128.0;
  static char *kwlist[] = {"file", "capacity", "error_rate", NULL};

  PyArg_ParseTupleAndKeywords(args,
			      kwargs,
			      "O|ld",
			      kwlist,
			      &source,
			      &capacity,
			      &error_rate);
  printf("ok\n");
  if (PyString_Check(source)) {
    printf("Path\n");
    path = PyString_AS_STRING(source);
    fd = open(path, O_CREAT, ~0);
    if (fd == -1)
      return PyErr_SetFromErrnoWithFilename(PyExc_IOError, path);
  } else {
    printf("Int\n");
    if (PyInt_Check(source)) {
      fd = PyInt_AsLong(source);
    } else {
      PyErr_SetString(PyExc_TypeError, "must provide a path string or int fileno\n");
    }
  }
  printf("Open(%s %d)\n", path, fd);
  
    
  PyObject *smbo = make_new_shared_memory_bloomfilter(type, dup(fd), capacity, error_rate);
  printf("And done\n");
  if (path)
    close(fd);

  return (PyObject *)smbo;
}

PyTypeObject SharedMemoryBloomfilterType = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "SharedMemoryBloomfilter", /* tp_name */
  sizeof(SharedMemoryBloomfilterObject), /*tp_basicsize*/
  0, /*tp_itemsize */
  (destructor)shared_memory_bloomfilter_type_dealloc, /* tp_dealloc */
  0, /* tp_print */
  0, /*tp_getattr*/
  0, /*tp_setattr*/
  0, /*tp_cmp*/
  0, /*tp_repr*/
  0, /* tp_as_number */
  &SharedMemoryBloomfilterObject_sequence_methods, /* tp_as_seqeunce */
  0, 
  (hashfunc)PyObject_HashNotImplemented, /*tp_hash */
  0, /* tp_call */
  0, /* tp_str */
  PyObject_GenericGetAttr, /* tp_getattro */
  0, /* tp_setattro */
  0, /* tp_as_buffer */
  Py_TPFLAGS_HAVE_SEQUENCE_IN,	/* tp_flags */
  0, /* tp_doc */
  0, /* tp_traverse */
  0, /* tp_clear */
  0, /* tp_richcompare */
  offsetof(SharedMemoryBloomfilterObject, weakreflist), /* tp_weaklistoffset */
  0, /* tp_iter */
  0, /* tp_iternext */
  shared_memory_bloomfilter_methods, /* tp_methods */
  0, /* tp_members */
  0, /* tp_genset */
  0, /* tp_base */
  0, /* tp_dict */
  0,				/* tp_descr_get */
  0,				/* tp_descr_set */
  0,				/* tp_dictoffset */
  (initproc)shared_memory_bloomfilter_init,		/* tp_init */
  PyType_GenericAlloc,		/* tp_alloc */
  shared_memory_bloomfilter_new,			/* tp_new */
  PyObject_GC_Del,		/* tp_free */
};

PyObject *
make_new_shared_memory_bloomfilter(PyTypeObject *type, int fd, uint64_t capacity, double error_rate) {
  SharedMemoryBloomfilterObject *smbo = PyObject_GC_New(SharedMemoryBloomfilterObject, &SharedMemoryBloomfilterType);;
  
  if (!(smbo->bf= create_bloomfilter(fd, capacity, error_rate))) {
    return NULL;
  }
  smbo->weakreflist = NULL;
  return smbo;
}

static PyMethodDef shared_memory_bloomfiltermodule_methods[] = {
  {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initshared_memory_bloomfilter(void) {
  PyObject *m = Py_InitModule("shared_memory_bloomfilter", shared_memory_bloomfiltermodule_methods);
  Py_INCREF(&SharedMemoryBloomfilterType);
  PyModule_AddObject(m, "SharedMemoryBloomFilter", (PyObject *)&SharedMemoryBloomfilterType);
};