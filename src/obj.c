/* Object pool and list (port of fn_04034 / fn_040b6 / fn_0410e / fn_04094) */
#include "game.h"

#define MAX_OBJ 80

static Obj pool[MAX_OBJ];
static Obj *free_list;
static Obj *head, *tail;

void obj_init(void)
{
    int i;
    free_list = NULL;
    for (i = MAX_OBJ - 1; i >= 0; i--) {
        pool[i].active = 0;
        pool[i].next = free_list;
        free_list = &pool[i];
    }
    head = tail = NULL;
}

/* fn_040b6: new objects are appended, so they run (and draw) after older ones */
Obj *obj_alloc(void)
{
    Obj *o = free_list;
    u8 *p;
    u16 i;
    if (!o)
        return NULL;
    free_list = o->next;
    for (p = (u8 *)o, i = 0; i < sizeof(Obj); i++)
        p[i] = 0;
    o->active = 1;
    if (tail)
        tail->next = o;
    else
        head = o;
    tail = o;
    return o;
}

/* fn_0410e: deferred - the object is unlinked by obj_update_all */
void obj_free(Obj *o)
{
    o->active = 0;
}

/* fn_04094 */
void obj_update_all(void)
{
    Obj *o, *prev = NULL, *next;
    for (o = head; o; o = o->next)
        if (o->active)
            o->update(o);
    for (o = head; o; o = next) {
        next = o->next;
        if (!o->active) {
            if (prev)
                prev->next = next;
            else
                head = next;
            if (tail == o)
                tail = prev;
            o->next = free_list;
            free_list = o;
        } else {
            prev = o;
        }
    }
}

/* fn_2ca2c on-screen test: 0 <= x,y < $4400 (unsigned compare) */
int obj_onscreen(const Obj *o)
{
    return (u16)o->x < 0x4400 && (u16)o->y < 0x4400;
}
