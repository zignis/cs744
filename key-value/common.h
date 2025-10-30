#define MAX_CHUNK_SIZE 1024

typedef enum {
  CONNECT,
  DISCONNECT,
  // Crud
  CREATE,
  UPDATE,
  READ,
  DELETE
} CommandKind;
