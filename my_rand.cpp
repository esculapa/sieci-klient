#include <stdlib.h>

#define VAR 279410273
#define MOD 4294967291

static int32_t seed, previous_rand;
static int i;

void set_seed(int32_t new_seed){
  seed = new_seed;
  i = 0;
}

int32_t my_rand(){
  if (i == 0){
    previous_rand = seed;
    return seed;
  }
  long long random_value = (((previous_rand) * VAR) % MOD + MOD) % MOD;
  return (int32_t)random_value;
}
