#include "common.h"
#include "Pools.h"
#include "Lists.h"

#include <stdlib.h>

// A full pool must abort loudly. On 2002 counts the pools always had room, so
// the operators used assert() as a check; in a release build that check is
// compiled out and the caller writes to the nil node.
static void
ListPoolFull(const char *pool)
{
	Error("The %s pool is full; the world holds more list nodes than it was sized for. "
	      "Raise the pool size in src/core/config.h.", pool);
	abort();
}

void*
CPtrNode::operator new(size_t){
	CPtrNode *node = CPools::GetPtrNodePool()->New();
	if (node == nil)
		ListPoolFull("PtrNode");
	return node;
}

void
CPtrNode::operator delete(void *p, size_t){
	CPools::GetPtrNodePool()->Delete((CPtrNode*)p);
}

void*
CEntryInfoNode::operator new(size_t){
	CEntryInfoNode *node = CPools::GetEntryInfoNodePool()->New();
	if (node == nil)
		ListPoolFull("EntryInfoNode");
	return node;
}
void
CEntryInfoNode::operator delete(void *p, size_t){
	CPools::GetEntryInfoNodePool()->Delete((CEntryInfoNode*)p);
}
