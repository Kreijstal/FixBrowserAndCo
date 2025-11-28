/*
 * FixBrowser v0.4 - https://www.fixbrowser.org/
 * Copyright (c) 2018-2025 Martin Dvorak <jezek2@advel.cz>
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, 
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

// note: synchronize with script code
enum {
   ELEM_type,
   ELEM_parent,
   ELEM_first_child,
   ELEM_last_child,
   ELEM_prev,
   ELEM_next,
   ELEM_attrs,
   ELEM_data,
   ELEM_class_set,
   ELEM_SIZE
};

// note: synchronize with script code
enum {
   SEL_TYPE,
   SEL_ID,
   SEL_CLASS,
   SEL_ATTRIB,
   SEL_ATTRIB_PREFIX,
   SEL_ATTRIB_SUFFIX,
   SEL_ATTRIB_SUBSTRING,
   SEL_ATTRIB_EXACT,
   SEL_ATTRIB_INCLUDE,
   SEL_ATTRIB_DASH,
   SEL_PSEUDO_ELEMENT,
   SEL_PSEUDO_CLASS,
   SEL_FUNCTION_IDENT,
   SEL_FUNCTION_STRING,
   SEL_FUNCTION_ANB,
   SEL_NOT,
   SEL_SEQUENCE,

   COMB_DESCENDANT,
   COMB_CHILD,
   COMB_NEXT_SIBLING,
   COMB_SUBSEQUENT_SIBLING
};

// note: synchronize with script code
enum {
   SELECTOR_type = 0,

   // type selector:
   SELECTOR_elem_namespace = 1,
   SELECTOR_elem_name = 2,

   // id selector:
   SELECTOR_id_name = 1,

   // class selector:
   SELECTOR_class_name = 1,

   // attrib selector:
   SELECTOR_attrib_namespace = 1,
   SELECTOR_attrib_name = 2,
   SELECTOR_attrib_value = 3,

   // pseudo selector:
   SELECTOR_pseudo_name = 1,

   // function selector:
   SELECTOR_func_name = 1,
   SELECTOR_func_expr = 2,

   // not selector:
   SELECTOR_not_selector = 1,

   // selector sequence:
   SELECTOR_selectors = 1,

   // combinators:
   SELECTOR_first = 1,
   SELECTOR_second = 2,
   
   SELECTOR_SIZE = 4
};

typedef struct Object {
   void (*free)(void *p);
   struct Object *alloc_next;
} Object;

typedef struct Attribute {
   char *name;
   char *value;
   struct Attribute *next;
} Attribute;

typedef struct Class {
   char *value;
   struct Class *next;
} Class;

typedef struct Element {
   Object obj;
   char *type;
   struct Element *parent;
   struct Element *first_child;
   struct Element *last_child;
   struct Element *prev;
   struct Element *next;
   struct Attribute *attrs;
   struct Class *class_set;
   int data_len;
} Element;

typedef struct Selector {
   Object obj;
   int type;
   char *name;
   char *value;
   int num_selectors;
   struct Selector **selectors;
} Selector;

typedef struct {
   Element *document;
   Object *object_alloc;
   Object **object_map;
   int object_map_size;
} Context;

#define NUM_HANDLE_TYPES 1
#define HANDLE_TYPE_CONTEXT (handles_offset+0)

static volatile int handles_offset = 0;


static void free_element(void *p)
{
   Element *elem = p;
   Attribute *attr, *attr_next;
   Class *cls, *cls_next;
   
   free(elem->type);
   for (attr = elem->attrs; attr; attr = attr_next) {
      attr_next = attr->next;
      free(attr->name);
      free(attr->value);
      free(attr);
   }
   for (cls = elem->class_set; cls; cls = cls_next) {
      cls_next = cls->next;
      free(cls->value);
      free(cls);
   }
   free(elem);
}


static void free_selector(void *p)
{
   Selector *sel = p;

   free(sel->name);
   free(sel->value);
   free(sel->selectors);
   free(sel);
}


static void free_context(void *p)
{
   Context *ctx = p;
   Object *obj, *obj_next;

   for (obj = ctx->object_alloc; obj; obj = obj_next) {
      obj_next = obj->alloc_next;
      obj->free(obj);
   }

   free(ctx);
}


static int expand_object_map(Context *ctx, int value)
{
   Object **new_object_map;
   int new_cap;

   new_cap = ctx->object_map_size;
   while (value >= new_cap) {
      new_cap *= 2;
   }
   new_object_map = realloc(ctx->object_map, new_cap * sizeof(Object *));
   if (!new_object_map) {
      return 0;
   }
   memset(&new_object_map[ctx->object_map_size], 0, (new_cap - ctx->object_map_size) * sizeof(Object *));
   ctx->object_map = new_object_map;
   ctx->object_map_size = new_cap;
   return 1;
}


static Element *get_element(Context *ctx, Heap *heap, Value *error, Value element_value)
{
   Element *elem = NULL;
   Attribute *attr;
   Class *cls;
   Value values[ELEM_SIZE], key, value;
   int err, pos;

   if (element_value.value > 0 && element_value.value < ctx->object_map_size && ctx->object_map[element_value.value]) {
      elem = (Element *)ctx->object_map[element_value.value];
      if (elem->obj.free != free_element) {
         *error = fixscript_create_error_string(heap, "internal error: invalid element object");
         return NULL;
      }
      return elem;
   }

   err = fixscript_get_array_range(heap, element_value, 0, ELEM_SIZE, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (element_value.value >= ctx->object_map_size) {
      if (!expand_object_map(ctx, element_value.value)) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
   }

   elem = calloc(1, sizeof(Element));
   if (!elem) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   elem->obj.free = free_element;
   elem->obj.alloc_next = ctx->object_alloc;
   ctx->object_alloc = &elem->obj;
   ctx->object_map[element_value.value] = &elem->obj;

   err = fixscript_get_string(heap, values[ELEM_type], 0, -1, &elem->type, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (values[ELEM_parent].value) {
      elem->parent = get_element(ctx, heap, error, values[ELEM_parent]);
      if (!elem->parent) {
         goto error;
      }
   }

   if (values[ELEM_first_child].value) {
      elem->first_child = get_element(ctx, heap, error, values[ELEM_first_child]);
      if (!elem->first_child) {
         goto error;
      }
   }

   if (values[ELEM_last_child].value) {
      elem->last_child = get_element(ctx, heap, error, values[ELEM_last_child]);
      if (!elem->last_child) {
         goto error;
      }
   }

   if (values[ELEM_prev].value) {
      elem->prev = get_element(ctx, heap, error, values[ELEM_prev]);
      if (!elem->prev) {
         goto error;
      }
   }

   if (values[ELEM_next].value) {
      elem->next = get_element(ctx, heap, error, values[ELEM_next]);
      if (!elem->next) {
         goto error;
      }
   }

   if (values[ELEM_attrs].value) {
      pos = 0;
      while (fixscript_iter_hash(heap, values[ELEM_attrs], &key, &value, &pos)) {
         attr = calloc(1, sizeof(Attribute));
         if (!attr) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }

         attr->next = elem->attrs;
         elem->attrs = attr;

         err = fixscript_get_string(heap, key, 0, -1, &attr->name, NULL);
         if (!err) {
            err = fixscript_get_string(heap, value, 0, -1, &attr->value, NULL);
         }
         if (err) {
            fixscript_error(heap, error, err);
            goto error;
         }
      }
   }

   if (values[ELEM_class_set].value) {
      pos = 0;
      while (fixscript_iter_hash(heap, values[ELEM_class_set], &key, &value, &pos)) {
         cls = calloc(1, sizeof(Class));
         if (!cls) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }

         cls->next = elem->class_set;
         elem->class_set = cls;

         err = fixscript_get_string(heap, key, 0, -1, &cls->value, NULL);
         if (err) {
            fixscript_error(heap, error, err);
            goto error;
         }
      }
   }

   if (values[ELEM_data].value) {
      err = fixscript_get_array_length(heap, values[ELEM_data], &elem->data_len);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   return elem;

error:
   if (elem) {
      ctx->object_map[element_value.value] = NULL;
   }
   return NULL;
}


static const char *element_get_attr(Element *elem, const char *name)
{
   Attribute *attr;

   for (attr = elem->attrs; attr; attr = attr->next) {
      if (strcmp(attr->name, name) == 0) {
         return attr->value;
      }
   }
   return NULL;
}


static int element_has_class(Element *elem, const char *value)
{
   Class *cls;

   for (cls = elem->class_set; cls; cls = cls->next) {
      if (strcmp(cls->value, value) == 0) {
         return 1;
      }
   }
   return 0;
}


static Element *element_get_prev_tag(Element *elem)
{
   Element *e;

   e = elem->prev;
   while (e && e->type[0] == '#') {
      e = e->prev;
   }
   return e;
}


static Element *element_get_next_tag(Element *elem)
{
   Element *e;

   e = elem->next;
   while (e && e->type[0] == '#') {
      e = e->next;
   }
   return e;
}


static Selector *get_selector(Context *ctx, Heap *heap, Value *error, Value selector_value)
{
   Selector *sel = NULL;
   Value values[SELECTOR_SIZE], sel_val;
   char *p;
   int i, err, selector_size;

   if (selector_value.value > 0 && selector_value.value < ctx->object_map_size && ctx->object_map[selector_value.value]) {
      sel = (Selector *)ctx->object_map[selector_value.value];
      if (sel->obj.free != free_selector) {
         *error = fixscript_create_error_string(heap, "internal error: invalid selector object");
         return NULL;
      }
      return sel;
   }

   err = fixscript_get_array_length(heap, selector_value, &selector_size);
   if (!err && selector_size > SELECTOR_SIZE) {
      *error = fixscript_create_error_string(heap, "internal error: selector object contains more data");
      goto error;
   }
   if (!err) {
      memset(values, 0, sizeof(values));
      err = fixscript_get_array_range(heap, selector_value, 0, selector_size, values);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (selector_value.value >= ctx->object_map_size) {
      if (!expand_object_map(ctx, selector_value.value)) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
   }

   sel = calloc(1, sizeof(Selector));
   if (!sel) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   sel->obj.free = free_selector;
   sel->obj.alloc_next = ctx->object_alloc;
   ctx->object_alloc = &sel->obj;
   ctx->object_map[selector_value.value] = &sel->obj;

   sel->type = values[SELECTOR_type].value;

   switch (sel->type) {
      case SEL_TYPE:
         err = fixscript_get_string(heap, values[SELECTOR_elem_name], 0, -1, &sel->name, NULL);
         break;

      case SEL_ID:
         err = fixscript_get_string(heap, values[SELECTOR_id_name], 0, -1, &sel->name, NULL);
         break;

      case SEL_CLASS:
         err = fixscript_get_string(heap, values[SELECTOR_class_name], 0, -1, &sel->name, NULL);
         break;

      case SEL_ATTRIB:
         err = fixscript_get_string(heap, values[SELECTOR_attrib_name], 0, -1, &sel->name, NULL);
         break;

      case SEL_ATTRIB_PREFIX:
      case SEL_ATTRIB_SUFFIX:
      case SEL_ATTRIB_SUBSTRING:
      case SEL_ATTRIB_EXACT:
      case SEL_ATTRIB_INCLUDE:
      case SEL_ATTRIB_DASH:
         err = fixscript_get_string(heap, values[SELECTOR_attrib_name], 0, -1, &sel->name, NULL);
         if (!err) {
            err = fixscript_get_string(heap, values[SELECTOR_attrib_value], 0, -1, &sel->value, NULL);
         }
         break;

      case SEL_PSEUDO_ELEMENT:
         break;

      case SEL_PSEUDO_CLASS:
         err = fixscript_get_string(heap, values[SELECTOR_class_name], 0, -1, &sel->name, NULL);
         if (!err) {
            for (p=sel->name; *p; p++) {
               if (*p >= 'A' && *p <= 'Z') {
                  *p = *p - 'A' + 'a';
               }
            }
         }
         break;

      case SEL_FUNCTION_IDENT:
      case SEL_FUNCTION_STRING:
      case SEL_FUNCTION_ANB:
         break;

      case SEL_NOT:
         sel->num_selectors = 1;
         sel->selectors = calloc(sel->num_selectors, sizeof(Selector *));
         if (!sel->selectors) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }
         sel->selectors[0] = get_selector(ctx, heap, error, values[SELECTOR_not_selector]);
         if (!sel->selectors[0]) {
            goto error;
         }
         break;

      case SEL_SEQUENCE:
         err = fixscript_get_array_length(heap, values[SELECTOR_selectors], &sel->num_selectors);
         if (err) {
            sel->num_selectors = 0;
         }
         else {
            sel->selectors = calloc(sel->num_selectors, sizeof(Selector *));
            if (!sel->selectors) {
               fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
               goto error;
            }
            for (i=0; i<sel->num_selectors; i++) {
               err = fixscript_get_array_elem(heap, values[SELECTOR_selectors], i, &sel_val);
               if (err) break;
               sel->selectors[i] = get_selector(ctx, heap, error, sel_val);
               if (!sel->selectors[i]) {
                  goto error;
               }
            }
         }
         break;

      case COMB_DESCENDANT:
      case COMB_CHILD:
      case COMB_NEXT_SIBLING:
      case COMB_SUBSEQUENT_SIBLING:
         sel->num_selectors = 2;
         sel->selectors = calloc(sel->num_selectors, sizeof(Selector *));
         if (!sel->selectors) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }
         sel->selectors[0] = get_selector(ctx, heap, error, values[SELECTOR_first]);
         if (sel->selectors[0]) {
            sel->selectors[1] = get_selector(ctx, heap, error, values[SELECTOR_second]);
         }
         if (!sel->selectors[0] || !sel->selectors[1]) {
            goto error;
         }
         break;
   }

   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   return sel;

error:
   if (sel) {
      ctx->object_map[selector_value.value] = NULL;
   }
   return NULL;
}


static int string_starts_with(const char *s, const char *prefix)
{
   return strncmp(s, prefix, strlen(prefix)) == 0;
}


static int string_ends_with(const char *s, const char *suffix)
{
   int len1 = strlen(s);
   int len2 = strlen(suffix);
   if (len1 < len2) return 0;
   return strcmp(s + (len1 - len2), suffix) == 0;
}


static int contains(const char *s, const char *match)
{
   int i, c, idx = 0, len1 = strlen(s), len2 = strlen(match), found = 1;

   for (i=0; i<len1; i++) {
      c = s[i];
      switch (c) {
         case ' ':
         case '\t':
         case '\n':
         case '\r':
         case 0x0C:
            if (found && idx == len2) {
               return 1;
            }
            idx = 0;
            found = 1;
            break;

         default:
            if (idx < len2) {
               if (c != match[idx]) {
                  found = 0;
               }
            }
            else {
               found = 0;
            }
            idx++;
            break;
      }
   }

   if (found && idx == len2) {
      return 1;
   }
   return 0;
}


static int match_selector(Element *element, Selector *selector)
{
   switch (selector->type) {
      case SEL_TYPE: {
         if (strcmp(selector->name, "*") == 0) {
            return 1;
         }
         if (strcmp(element->type, selector->name) == 0) {
            return 1;
         }
         return 0;
      }

      case SEL_ID: {
         const char *elem_value = element_get_attr(element, "id");
         if (!elem_value) return 0;

         return strcmp(elem_value, selector->name) == 0;
      }

      case SEL_CLASS: {
         return element_has_class(element, selector->name);
      }

      case SEL_ATTRIB: {
         return element_get_attr(element, selector->name) != NULL;
      }

      case SEL_ATTRIB_PREFIX: {
         const char *elem_value = element_get_attr(element, selector->name);
         if (!elem_value) return 0;
         
         return string_starts_with(elem_value, selector->value);
      }

      case SEL_ATTRIB_SUFFIX: {
         const char *elem_value = element_get_attr(element, selector->name);
         if (!elem_value) return 0;
         
         return string_ends_with(elem_value, selector->value);
      }

      case SEL_ATTRIB_SUBSTRING: {
         const char *elem_value = element_get_attr(element, selector->name);
         if (!elem_value) return 0;
         
         return strstr(elem_value, selector->value) != NULL;
      }

      case SEL_ATTRIB_EXACT: {
         const char *elem_value = element_get_attr(element, selector->name);
         if (!elem_value) return 0;
         
         return strcmp(elem_value, selector->value) == 0;
      }

      case SEL_ATTRIB_INCLUDE: {
         const char *elem_value = element_get_attr(element, selector->name);
         if (!elem_value) return 0;
         
         return contains(elem_value, selector->value) == 0;
      }

      case SEL_ATTRIB_DASH: {
         const char *elem_value = element_get_attr(element, selector->name);
         const char *match_value = selector->value;
         int match_len = strlen(match_value);
         if (!elem_value) return 0;
         
         if (strcmp(elem_value, match_value) == 0) return 1;
         if (strlen(elem_value) > match_len && string_starts_with(elem_value, match_value) && elem_value[match_len] == '-') {
            return 1;
         }
         return 0;
      }

      case SEL_PSEUDO_ELEMENT:
         return 0;

      case SEL_PSEUDO_CLASS: {
         switch (selector->name[0]) {
            case 'r':
               if (strcmp(selector->name, "root") == 0) {
                  Element *parent = element->parent;
                  if (parent && strcmp(parent->type, "#document") == 0) {
                     return 1;
                  }
                  return 0;
               }
               break;

            case 'e':
               if (strcmp(selector->name, "empty") == 0) {
                  Element *elem;
                  for (elem = element->first_child; elem; elem = elem->next) {
                     if (strcmp(elem->type, "#comment") == 0) {
                        continue;
                     }
                     if (strcmp(elem->type, "#text") == 0 && elem->data_len == 0) {
                        continue;
                     }
                     return 0;
                  }
                  return 1;
               }
               break;

            case 'l':
               if (strcmp(selector->name, "last-child") == 0) {
                  if (!element_get_next_tag(element)) {
                     return 1;
                  }
                  return 0;
               }
               if (strcmp(selector->name, "last-of-type") == 0) {
                  Element *elem;
                  const char *type = element->type;
                  for (elem = element_get_next_tag(element); elem; elem = element_get_next_tag(elem)) {
                     if (strcmp(elem->type, type) == 0) {
                        return 0;
                     }
                  }
                  return 1;
               }
               break;

            case 'o':
               if (strcmp(selector->name, "only-child") == 0) {
                  if (!element_get_prev_tag(element) && !element_get_next_tag(element)) {
                     return 1;
                  }
                  return 0;
               }
               if (strcmp(selector->name, "only-of-type") == 0) {
                  Element *elem;
                  const char *type = element->type;
                  for (elem = element_get_prev_tag(element); elem; elem = element_get_prev_tag(elem)) {
                     if (strcmp(elem->type, type) == 0) {
                        return 0;
                     }
                  }
                  for (elem = element_get_next_tag(element); elem; elem = element_get_next_tag(elem)) {
                     if (strcmp(elem->type, type) == 0) {
                        return 0;
                     }
                  }
                  return 1;
               }
               break;

            case 'f':
               if (strcmp(selector->name, "first-child") == 0) {
                  if (!element_get_prev_tag(element)) {
                     return 1;
                  }
                  return 0;
               }
               if (strcmp(selector->name, "first-of-type") == 0) {
                  Element *elem;
                  const char *type = element->type;
                  for (elem = element_get_prev_tag(element); elem; elem = element_get_prev_tag(elem)) {
                     if (strcmp(elem->type, type) == 0) {
                        return 0;
                     }
                  }
                  return 1;
               }
               break;
         }
         return 0;
      }

      case SEL_FUNCTION_IDENT:
      case SEL_FUNCTION_STRING:
      case SEL_FUNCTION_ANB:
         return 0;

      case SEL_NOT:
         return !match_selector(element, selector->selectors[0]);
      
      case SEL_SEQUENCE: {
         Selector **selectors = selector->selectors;
         int i, len = selector->num_selectors;
         for (i=0; i<len; i++) {
            if (!match_selector(element, selectors[i])) {
               return 0;
            }
         }
         return 1;
      }

      case COMB_DESCENDANT: {
         Element *parent;

         if (!match_selector(element, selector->selectors[1])) return 0;

         parent = element;
         for (;;) {
            parent = parent->parent;
            if (!parent || strcmp(parent->type, "#document") == 0) break;

            if (match_selector(parent, selector->selectors[0])) return 1;
         }
         return 0;
      }

      case COMB_CHILD: {
         Element *parent;

         if (!match_selector(element, selector->selectors[1])) return 0;

         parent = element->parent;
         if (!parent || strcmp(parent->type, "#document") == 0) {
            return 0;
         }
         return match_selector(parent, selector->selectors[0]);
      }

      case COMB_NEXT_SIBLING: {
         Element *prev;

         if (!match_selector(element, selector->selectors[1])) return 0;

         prev = element_get_prev_tag(element);
         if (!prev) return 0;

         return match_selector(prev, selector->selectors[0]);
      }

      case COMB_SUBSEQUENT_SIBLING: {
         Element *elem;

         if (!match_selector(element, selector->selectors[1])) return 0;

         for (elem = element_get_prev_tag(element); elem; elem = element_get_prev_tag(elem)) {
            if (match_selector(elem, selector->selectors[0])) return 1;
         }
         return 0;
      }
   }
   return 0;
}


static Value css_matcher_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Context *ctx;
   Value ret;

   ctx = calloc(1, sizeof(Context));
   if (!ctx) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ctx->object_map_size = 1024;
   ctx->object_map = calloc(ctx->object_map_size, sizeof(Object *));
   if (!ctx->object_map) {
      free(ctx);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ctx->document = get_element(ctx, heap, error, params[0]);
   if (!ctx->document) {
      free_context(ctx);
      return fixscript_int(0);
   }

   ret = fixscript_create_handle(heap, HANDLE_TYPE_CONTEXT, ctx, free_context);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value css_matcher_matches(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Context *ctx;
   Element *elem;
   Selector *sel;

   ctx = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CONTEXT, NULL);
   if (!ctx) {
      *error = fixscript_create_error_string(heap, "invalid CSS matcher handle");
      return fixscript_int(0);
   }

   elem = get_element(ctx, heap, error, params[1]);
   if (!elem) {
      return fixscript_int(0);
   }

   sel = get_selector(ctx, heap, error, params[2]);
   if (!sel) {
      return fixscript_int(0);
   }

   return fixscript_int(match_selector(elem, sel));
}


void register_css_functions(Heap *heap)
{
   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);

   fixscript_register_native_func(heap, "css_matcher_create#1", css_matcher_create, NULL);
   fixscript_register_native_func(heap, "css_matcher_matches#3", css_matcher_matches, NULL);
}
