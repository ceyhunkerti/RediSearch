#include "../types.h"
#include "vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SR_MAX_LEAF_SIZE 1000

typedef struct docNode {
  t_docId docId;
  float score;
  int size;
  struct docNode *left;
  struct docNode *right;
} DocNode;

typedef enum {
  StateNew,
  StateLeft,
  StateSelf,
  StateRight,
} IteratorState;

typedef struct docNodeIterator {
  DocNode *node;
  struct docNodeIterator *parent;
  IteratorState state;
} DocNodeIterator;
DocNodeIterator *DocNodeIterator_Next(DocNodeIterator *i);

DocNodeIterator *newDocNodeIterator(DocNode *n, DocNodeIterator *parent) {
  DocNodeIterator *ret = malloc(sizeof(DocNodeIterator));
  ret->node = n;
  ret->parent = parent;
  ret->state = StateNew;
  return ret;
}

DocNodeIterator *DocNode_Iterate(DocNode *n) {
  return DocNodeIterator_Next(newDocNodeIterator(n, NULL));
}

DocNodeIterator *DocNodeIterator_Next(DocNodeIterator *i) {
  // fmt.Printf("next %p: state %v, docId %d left %p right %p parent %p\n",
  // i.node, i.state, i.node.docId, i.node.left, i.node.right, parent)
  if (i->state == StateNew) {
    while (i->node->left) {
      i->state = StateLeft;
      i = newDocNodeIterator(i->node->left, i);
    }
  }

  if (i->state == StateLeft ||
      (i->state == StateNew && i->node->left == NULL)) {
    i->state = StateSelf;
    return i;
  }

  if (i->state == StateSelf) {
    i->state = StateRight;
    if (i->node->right != NULL) {
      i = newDocNodeIterator(i->node->right, i);
      return DocNodeIterator_Next(i);
    }
  }

  DocNodeIterator *parent = i->parent ? DocNodeIterator_Next(i->parent) : NULL;
  free(i);
  return parent;
}

DocNode *newDocNode(t_docId docId, float score) {
  DocNode *ret = malloc(sizeof(DocNode));
  ret->docId = docId;
  ret->score = score;
  ret->left = NULL;
  ret->size = 1;
  ret->right = NULL;
  return ret;
}

void DocNode_Add(DocNode *n, t_docId docId, float score) {
  DocNode *current = n;
  n->size++;
  while (current != NULL) {
    if (docId == current->docId) {
      return;
    }

    if (docId < current->docId) {
      if (current->left == NULL) {
        current->left = newDocNode(docId, score);
        return;
      }
      current = current->left;
    } else {
      if (current->right == NULL) {
        current->right = newDocNode(docId, score);
        return;
      }
      current = current->right;
    }
  }
}

void DocNode_Split(DocNode *n, float splitPoint, DocNode **left,
                   DocNode **right) {
  DocNodeIterator *it = DocNode_Iterate(n);
  while (it) {
    DocNode *current = it->node;
    if (current->score < splitPoint) {
      if (*left == NULL) {
        *left = newDocNode(current->docId, current->score);
      } else {
        DocNode_Add(*left, current->docId, current->score);
      }
    } else {
      if (*right == NULL) {
        *right = newDocNode(current->docId, current->score);
      } else {
        DocNode_Add(*right, current->docId, current->score);
      }
    }

    it = DocNodeIterator_Next(it);
  };
}

typedef struct leaf {
  DocNode *doctree;
  float min;
  float max;
  // TODO: make this a set/bloom filter
  int distinct;  // map[float32]struct{}
} Leaf;


Leaf *newLeaf(DocNode *docs, int size, float min, float max) {
    Leaf *ret = malloc(sizeof(Leaf));
    ret->doctree = docs;
    ret->distinct = size;
    ret->min = min;
    ret->max = max;

    return ret;
}


void Leaf_Split(Leaf *l, Leaf **left, Leaf **right) {
  float split = (l->min + l->max) / 2;

  DocNode *ld = NULL, *rd = NULL;
  DocNode_Split(l->doctree, split, &ld, &rd);
  
  *left = newLeaf(ld, ld ? ld->size : 0, l->min, split);
  *right = newLeaf(rd, rd ? rd->size : 0, split, l->max);
  
}

void Leaf_Add(Leaf *l, t_docId docId, float score) {
  l->distinct++;
  DocNode_Add(l->doctree, docId, score);

  if (score < l->min) l->min = score;
  if (score > l->max) l->max = score;
}

typedef struct scoreNode {
  float score;
  struct scoreNode *left;
  struct scoreNode *right;
  Leaf *leaf;
} ScoreNode;

ScoreNode *newScoreNode(Leaf *l) {
  ScoreNode *ret = malloc(sizeof(ScoreNode));
  ret->leaf = l;
  ret->left = NULL;
  ret->right = NULL;
  ret->score = 0;
  return ret;
}

void ScoreNode_Add(ScoreNode *n, t_docId docId, float score) {
  while (n && !n->leaf) {
    n = score < n->score ? n->left : n->right;
  }

  Leaf_Add(n->leaf, docId, score);

  if (n->leaf->distinct > SR_MAX_LEAF_SIZE) {
    //printf("Splitting node with leaf %p\n", n->leaf);

    Leaf *ll = NULL, *rl = NULL;
    Leaf_Split(n->leaf, &ll, &rl);

    free(n->leaf);
    n->leaf = NULL;
    n->score = ll->max;

    n->right = newScoreNode(rl);
    n->left = newScoreNode(ll);
  }
}

Vector *ScoreNode_FindRange(ScoreNode *n, float min, float max) {
  Vector *leaves = NewVector(Leaf *, 8);

  ScoreNode *vmin = n, *vmax = n;

  while (vmin == vmax && vmin) {
    vmin = min < vmin->score ? vmin->left : vmin->right;
    vmax = max < vmax->score ? vmax->left : vmax->right;
  }

  Vector *stack = NewVector(ScoreNode *, 8);

  // put on the stack all right trees of our path to the minimum node
  while (vmin && !vmin->leaf) {
    if (vmin->right && min < vmin->score) {
      Vector_Push(stack, vmin->right);
    }
    vmin = min < vmin->score ? vmin->left : vmin->right;
  }
  // put on the stack all left trees of our path to the maximum node
  while (vmax && !vmax->leaf) {
    if (vmax->left && max >= vmax->score) {
      Vector_Push(stack, vmax->left);
    }
    vmax = max < vmax->score ? vmax->left : vmax->right;
  }

  if (vmin) Vector_Push(leaves, vmin->leaf);
  if (vmax) Vector_Push(leaves, vmax->leaf);

  while (Vector_Size(stack)) {
    ScoreNode *n;
    if (!Vector_Pop(stack, &n)) break;
    if (!n) continue;

    if (n->leaf) Vector_Push(leaves, n->leaf);

    if (n->left) Vector_Push(stack, n->left);
    if (n->right) Vector_Push(stack, n->right);
  }

  Vector_Free(stack);

  printf("found %d leaves\n", Vector_Size(leaves));
  return leaves;
}


int main(int argc, char **argv) {

    ScoreNode *root = newScoreNode(newLeaf(newDocNode(0,0), 0, 0, 0));
    
    int N = 1000000;
    for (int i = 0; i < N; i++) {
        ScoreNode_Add(root, i, (float)(rand()%100000));
    }
     
    	int c = 0;
  
    struct timespec start_time, end_time;
  
    clock_gettime(CLOCK_REALTIME, &start_time);

    Vector *leaves = ScoreNode_FindRange(root, 100, 30000);
    int n = (int)Vector_Size(leaves);
    DocNodeIterator *iters[n];
    for (int i = 0; i <n; i++) {
        Leaf *l;
        Vector_Get(leaves, i, &l);
        iters[i] = DocNode_Iterate(l->doctree);
    }


    
    
    
	while (1) {
        DocNodeIterator *min = NULL;
	    int minIdx = -1;

        for (int i = 0; i < n; i++) {
		    DocNodeIterator *it = iters[i];
            if (!it) continue;
            if (!min || it->node->docId < min->node->docId) {
               min = it;
               minIdx = i;
            }
        }

        // no more iterators to read
        if (!min) break;
        
        //printf("%d => %f\n", min->node->docId, min->node->score);
        
                // fmt.Println(min.node.docId, min.node.score)
        iters[minIdx] = DocNodeIterator_Next(min);
        c++;
      }
      
      clock_gettime(CLOCK_REALTIME, &end_time);
     long diffInNanos = end_time.tv_nsec - start_time.tv_nsec;
    

       printf("got %d nodes, Time elapsed: %ldnano\n", c, diffInNanos);
      //printf("got %d nodes\n", c);
  
}