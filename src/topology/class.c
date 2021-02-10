/*
  Copyright(c) 2020 Intel Corporation
  All rights reserved.

  This library is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
*/
#define TPLG_DEBUG

#include "list.h"
#include "local.h"
#include "tplg_local.h"
#include "tplg2_local.h"
#include <ctype.h>

/* mapping of widget text names to types */
static const struct map_elem class_map[] = {
	{"Base", SND_TPLG_CLASS_TYPE_BASE},
	{"Pipeline", SND_TPLG_CLASS_TYPE_PIPELINE},
	{"Component", SND_TPLG_CLASS_TYPE_COMPONENT},
	{"Control", SND_TPLG_CLASS_TYPE_CONTROL},
	{"Dai", SND_TPLG_CLASS_TYPE_DAI},
};

int lookup_class_type(const char *c)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(class_map); i++) {
		if (strcmp(class_map[i].name, c) == 0)
			return class_map[i].id;
	}

	return -EINVAL;
}

static int tplg_parse_constraint_valid_values(snd_tplg_t *tplg, snd_config_t *cfg,
					      struct attribute_constraint *c,
					      char *name)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int err;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute_ref *v;
		const char *id, *s;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid reference value for '%s'\n", name);
			return -EINVAL;
		}

		err = snd_config_get_string(n, &s);
		if (err < 0) {
			SNDERR("Invalid value for '%s'\n", name);
			return err;
		}

		v = calloc(1, sizeof(*v));

		if (c->value_ref) {
			struct tplg_elem *token_elem;

			v->string = s;

			/* get reference token elem */
			token_elem = tplg_elem_lookup(&tplg->token_list,
						      c->value_ref,
						      SND_TPLG_TYPE_TOKEN, SND_TPLG_INDEX_ALL);
			if (!token_elem) {
				SNDERR("No valid token elem for ref '%s'\n",
					c->value_ref);
				free(v);
				return -EINVAL;
			}

			v->value = get_token_value(s, token_elem->tokens);
		} else {
			v->string = s;
			v->value = -EINVAL;
		}

		list_add(&v->list, &c->value_list);
	}

	return 0;
}

static int tplg_parse_class_constraints(snd_tplg_t *tplg, snd_config_t *cfg,
					struct attribute_constraint *c, char *name)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int err;

	snd_config_for_each(i, next, cfg) {
		const char *id, *s;
		long v;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (!strcmp(id, "min")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid min constraint for %s\n", name);
				return err;
			}
			c->min = v;
			continue;
		}

		if (!strcmp(id, "max")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid min constraint for %s\n", name);
				return err;
			}
			c->max = v;
			continue;
		}

		if (!strcmp(id, "value_ref")) {
			err = snd_config_get_string(n, &s);
			if (err < 0) {
				SNDERR("Invalid value ref for %s\n", name);
				return err;
			}
			c->value_ref = s;
			continue;
		}

		if (!strcmp(id, "values")) {
			err = tplg_parse_constraint_valid_values(tplg, n, c, name);
			if (err < 0) {
				SNDERR("Error parsing valid values for %s\n", name);
				return err;
			}
			continue;
		}
	}

	return 0;
}

static bool tplg_class_attribute_sanity_check(struct tplg_class *class)
{
	struct list_head *pos;

	list_for_each(pos, &class->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		/* if tuple is mandatory and immutable, it must have a value */
		if ((attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY) &&
		    (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE) &&
		    !attr->found) {
			SNDERR("Mandatory immutable attribute '%s' not provide for class '%s'",
			       attr->name, class->name);
			return false;
		}
	}

	return true;
}

static int tplg_parse_attribute_compound_value(snd_config_t *cfg, struct tplg_attribute *attr)
{
	snd_config_iterator_t i, next;
	struct list_head *pos;
	snd_config_t *n;

	snd_config_for_each(i, next, cfg) {
		const char *id, *s;
		bool found = false;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid cfg id for attribute %s\n", attr->name);
			return -EINVAL;
		}

		if (snd_config_get_string(n, &s) < 0) {
			SNDERR("invalid string for attribute %s\n", attr->name);
			return -EINVAL;
		}

		if (list_empty(&attr->constraint.value_list))
			continue;


		list_for_each(pos, &attr->constraint.value_list) {
			struct tplg_attribute_ref *v;

			v = list_entry(pos, struct tplg_attribute_ref, list);
			if (!strcmp(s, v->string)) {
				found = true;
				break;
			}
		}

		if (!found) {
			SNDERR("Invalid value %s for attribute %s\n", s, attr->name);
			return -EINVAL;
		}
	}

	return 0;
}

static struct tplg_attribute *tplg_get_attribute_by_name(struct list_head *list, const char *name)
{
	struct list_head *pos;

	list_for_each(pos, list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, name))
			return attr;
	}

	return NULL;
}

static int tplg_parse_class_attribute_category(snd_config_t *cfg, struct tplg_class *class,
					       int category)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute *attr;
		const char *id;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_string(n, &id) < 0) {
			SNDERR("invalid attribute category name for class %s\n", class->name);
			return -EINVAL;
		}

		attr = tplg_get_attribute_by_name(&class->attribute_list, id);
		if (!attr)
			continue;

		attr->constraint.mask |= category;
	}

	return 0;
}

static int tplg_parse_class_attribute_categories(snd_config_t *cfg, struct tplg_class *class)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int category = 0;
	int ret;

	snd_config_for_each(i, next, cfg) {
		const char *id;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid attribute category for class %s\n", class->name);
			return -EINVAL;
		}

		if (!strcmp(id, "mandatory"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY;

		if (!strcmp(id, "immutable"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE;

		if (!strcmp(id, "deprecated"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED;

		if (!category)
			continue;

		ret = tplg_parse_class_attribute_category(n, class, category);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int tplg_parse_attribute_value(snd_config_t *cfg, struct list_head *list)
{
	snd_config_type_t type = snd_config_get_type(cfg);
	struct tplg_attribute *attr = NULL;
	struct list_head *pos;
	bool found = false;
	int err;
	const char *s, *id;

	if (snd_config_get_id(cfg, &id) < 0) {
		SNDERR("No name for attribute\n");
		return -EINVAL;
	}

	list_for_each(pos, list) {
		attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, id)) {
			found = true;
			break;
		}
	}

	if (!found)
		return 0;

	attr->cfg = cfg;

	/* check if it is a reference to an argument */
	if (snd_config_get_string(cfg, &s) < 0)
		goto value;

	/* save arg reference and return */
	if (s[0] == '$') {
		snd_strlcpy(attr->ref, s + 1, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		snd_strlcpy(attr->value.string, s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		attr->found = true;
		attr->type = SND_CONFIG_TYPE_STRING;
		return 0;
	}

value:
	/* parse value */
	switch (type) {
	case SND_CONFIG_TYPE_INTEGER:
	{
		long v;

		err = snd_config_get_integer(cfg, &v);
		assert(err >= 0);

		if (v < attr->constraint.min || v > attr->constraint.max) {
			SNDERR("Value %d out of range for attribute %s\n", v, attr->name);
			return -EINVAL;
		}
		attr->value.integer = v;
		break;
	}
	case SND_CONFIG_TYPE_INTEGER64:
	{
		long long v;

		err = snd_config_get_integer64(cfg, &v);
		assert(err >= 0);
		if (v < attr->constraint.min || v > attr->constraint.max) {
			SNDERR("Value %ld out of range for attribute %s\n", v, attr->name);
			return -EINVAL;
		}

		attr->value.integer64 = v;
		break;
	}
	case SND_CONFIG_TYPE_STRING:
	{
		struct list_head *pos;
		const char *s;

		err = snd_config_get_string(cfg, &s);
		assert(err >= 0);

		/* attributes with no pre-defined value references */
		if (list_empty(&attr->constraint.value_list)) {
			if (!strcmp(s, "true")) {
				attr->value.integer = 1;
				attr->type = SND_CONFIG_TYPE_INTEGER;
				attr->found = true;
				return 0;
			} else if (!strcmp(s, "false")) {
				attr->value.integer = 0;
				attr->type = SND_CONFIG_TYPE_INTEGER;
				attr->found = true;
				return 0;
			}

			snd_strlcpy(attr->value.string, s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			break;
		}
		
		list_for_each(pos, &attr->constraint.value_list) {
			struct tplg_attribute_ref *v;

			v = list_entry(pos, struct tplg_attribute_ref, list);

			if (!strcmp(s, v->string)) {
				if (v->value != -EINVAL) {
					attr->value.integer = v->value;
					attr->type = SND_CONFIG_TYPE_INTEGER;
				} else {
					snd_strlcpy(attr->value.string, v->string,
						    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
					attr->type = type;
				}
				attr->found = true;
				return 0;
			}
		}

		SNDERR("Invalid value %s for attribute %s\n", s, attr->name);		
		return -EINVAL;
	}
	case SND_CONFIG_TYPE_REAL:
	{
		double d;

		err = snd_config_get_real(cfg, &d);
		assert(err >= 0);
		attr->value.d = d;
		break;
	}
	case SND_CONFIG_TYPE_COMPOUND:
		err = tplg_parse_attribute_compound_value(cfg, attr);
		if (err < 0)
			return err;
		break;
	default:
		SNDERR("Unsupported type %d for attribute %s\n", type, attr->name);
		return -EINVAL;
	}

	attr->type = type;
	attr->found = true;

	return 0;
}

static int tplg_parse_class_attribute(snd_tplg_t *tplg, snd_config_t *cfg,
				      struct tplg_attribute *attr)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (!strcmp(id, "constraints")) {	
			ret = tplg_parse_class_constraints(tplg, n, &attr->constraint,
								     attr->name);
			if (ret < 0) {
				SNDERR("Error parsing constraints for %s\n", attr->name);
				return -EINVAL;
			}
			continue;
		}

		if (!strcmp(id, "token_ref")) {
			const char *s;

			if (snd_config_get_string(n, &s) < 0) {
				SNDERR("invalid token_ref for attribute %s\n", attr->name);
				return -EINVAL;
			}

			snd_strlcpy(attr->token_ref, s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			continue;
		}
	}

	return 0;
}

static int tplg_parse_class_attributes(snd_tplg_t *tplg, snd_config_t *cfg,
				       struct tplg_class *class, int type)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret, j = 0;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute *attr;

		attr = calloc(1, sizeof(*attr));
		if (!attr)
			return -ENOMEM;
		attr->param_type = type;
		if (type == TPLG_CLASS_PARAM_TYPE_ARGUMENT)
			j++;

		INIT_LIST_HEAD(&attr->constraint.value_list);
		attr->constraint.min = INT_MIN;
		attr->constraint.max = INT_MAX;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* set attribute name */
		snd_strlcpy(attr->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

		ret = tplg_parse_class_attribute(tplg, n, attr);
		if (ret < 0)
			return ret;

		/* add to class attribute list */
		list_add_tail(&attr->list, &class->attribute_list);
	}

	if (type == TPLG_CLASS_PARAM_TYPE_ARGUMENT)
		class->num_args = j;
	return 0;
}

static struct tplg_elem *tplg_class_elem(snd_tplg_t *tplg, snd_config_t *cfg, int type)
{
	struct tplg_class *class;
	struct tplg_elem *elem;
	const char *id;

	if (snd_config_get_id(cfg, &id) < 0)
		return NULL;

	elem = tplg_elem_new_common(tplg, cfg, NULL, SND_TPLG_TYPE_CLASS);
	if (!elem)
		return NULL;

	class = calloc(1, sizeof(*class));
	if (!class)
		return NULL;

	class->type = type;
	snd_strlcpy(class->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	INIT_LIST_HEAD(&class->attribute_list);
	INIT_LIST_HEAD(&class->object_list);
	INIT_LIST_HEAD(&class->ref_object_list);
	elem->class = class;

	return elem;
}

static int tplg_process_attributes(snd_config_t *cfg, struct tplg_object *object)
{
	snd_config_iterator_t i, next;
	struct list_head *pos;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		list_for_each(pos, &object->attribute_list) {
			struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

			/* cannot update immutable attributes */
			if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE)
				continue;

			/* copy new value based on type */
			if (!strcmp(id, attr->name)) {
				ret = tplg_parse_attribute_value(n, &object->attribute_list);
				if (ret < 0) {
					SNDERR("Error parsing attribute %s value: %d\n",
					       attr->name, ret);
					return ret;
				}

				attr->found = true;
				break;
			}
		}
	}

	/* check if all mandatory attributes (but not immutable) are found */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if ((attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY) &&
		    !(attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE) &&
		    !attr->found) {
			SNDERR("Mandatory attribute %s not found\n", attr->name);
			return -EINVAL;
		}
	}

	return 0;
}

struct tplg_object *
tplg_create_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_class *class,
		   struct tplg_object *parent, struct list_head *list);

int tplg_create_child_object(snd_tplg_t *tplg, snd_config_t *cfg,
			     struct tplg_elem *class_elem,
			     struct tplg_object *parent, struct list_head *list)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;

	snd_config_for_each(i, next, cfg) {
		struct tplg_object *object;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		object = tplg_create_object(tplg, n, class_elem->class, parent, list);
		if (!object) {
			SNDERR("Error creating child %s for parent %s\n", id, parent->name);
			return -EINVAL;
		}
	}

	return 0;
}

static int tplg_create_child_objects(snd_tplg_t *tplg, snd_config_t *cfg,
				   struct tplg_object *parent)
{
	snd_config_iterator_t i, next;
	struct tplg_elem *class_elem;
	struct list_head *pos;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		bool found = false;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* check if it is an attribute, if so skip */
		list_for_each(pos, &parent->attribute_list) {
			struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

			if (!strcmp(attr->name, id)) {
				found = true;
				break;
			}
		}

		if (found)
			continue;

		/* check if it is an object */
		class_elem = tplg_elem_lookup(&tplg->class_list, id,
					      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);

		/* create object */
		if (class_elem) {
			ret = tplg_create_child_object(tplg, n, class_elem, parent,
						  &parent->object_list);
			if (ret < 0) {
				SNDERR("Error creating object type %s\n", class_elem->id);
				return ret;
			}
		}
	}

	return 0;
}

static void tplg_update_object_name_from_args(struct tplg_object *object)
{
	struct list_head *pos;
	char string[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int avail = SNDRV_CTL_ELEM_ID_NAME_MAXLEN;
	int len = 0, i = 0;

	snd_strlcpy(string, object->class_name, avail);
	avail -= strlen(object->class_name);
	len += strlen(object->class_name);

	snd_strlcpy(string + len, ".", avail);
	avail--;
	len++;

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		char arg_value[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

		if (i >= object->num_args)
			break;

		switch (attr->type) {
		case SND_CONFIG_TYPE_INTEGER:
			snprintf(arg_value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%ld",
				 attr->value.integer);
			break;
		case SND_CONFIG_TYPE_STRING:
			snprintf(arg_value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s",
				 attr->value.string);
			break;
		default:
			break;
		}

		snd_strlcpy(string + len, arg_value, avail);
		avail -= strlen(arg_value);
		len += strlen(arg_value);

		if (i < (object->num_args - 1)) {
			snd_strlcpy(string + len, ".", avail);
			avail--;
			len++;
		}
		i++;
	}

	snd_strlcpy(object->name, string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
}

static int tplg_update_string_from_attributes(struct tplg_object *object, char *string)
{
	struct list_head *pos;
	char new_id[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int avail = SNDRV_CTL_ELEM_ID_NAME_MAXLEN;
	int len = 0, i = 0;
	char *old_id = strdup(string);
	char *temp;

	if (!old_id)
		return -ENOMEM;

	while ((temp = strsep(&old_id, "."))) {
		bool found = false;

		if (i > 0) {
			snd_strlcpy(new_id + len, ".", avail);
			avail--;
			len++;
		}

		list_for_each(pos, &object->attribute_list) {
			struct tplg_attribute *attr =  list_entry(pos, struct tplg_attribute, list);

			if (temp[0] == '$' && !strcmp(temp + 1, attr->name)) {
				char arg_value[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

				if (!attr->found)
					break;
				if (attr->type == SND_CONFIG_TYPE_STRING)
					snprintf(arg_value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s",
						 attr->value.string);
				if(attr->type == SND_CONFIG_TYPE_INTEGER)
					snprintf(arg_value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%ld",
						 attr->value.integer);
				snd_strlcpy(new_id + len, arg_value, avail);
				avail -= strlen(arg_value);
				len += strlen(arg_value);
				found = true;
				break;
			}
		}
		if (!found) {
			snd_strlcpy(new_id + len, temp, avail);
			avail -= strlen(temp);
			len += strlen(temp);
		}
		i++;
	}

	snd_strlcpy(string, new_id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	free(old_id);

	return 0;
}


static int tplg_update_attributes_from_parent(struct tplg_object *object,
					    struct tplg_object *ref_object)
{
	struct list_head *pos;
	int ret;

	/* update comp attribute values from pipeline args */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr =  list_entry(pos, struct tplg_attribute, list);

		if (strcmp(attr->ref, "")) {
			struct list_head *pos1;

			list_for_each(pos1, &ref_object->attribute_list) {
				struct tplg_attribute *ref_attr;

				ref_attr = list_entry(pos1, struct tplg_attribute, list);
				if (!ref_attr->found)
					continue;

				if (!strcmp(attr->ref, ref_attr->name)) {
					switch (ref_attr->type) {
					case SND_CONFIG_TYPE_INTEGER:
						attr->value.integer = ref_attr->value.integer;
						attr->type = ref_attr->type;
						break;
					case SND_CONFIG_TYPE_INTEGER64:
						attr->value.integer64 = ref_attr->value.integer64;
						attr->type = ref_attr->type;
						break;
					case SND_CONFIG_TYPE_STRING:
						snd_strlcpy(attr->value.string,
							    ref_attr->value.string,
							    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
						attr->type = ref_attr->type;
						break;
					case SND_CONFIG_TYPE_REAL:
						attr->value.d = ref_attr->value.d;
						attr->type = ref_attr->type;
						break;
					default:
						SNDERR("Unsupported type %d for attribute %s\n",
							attr->type, attr->name);
						return -EINVAL;
					}
				}
			}
		}

		if (attr->type != SND_CONFIG_TYPE_STRING)
			continue;

		/* otherwise update string attribute values with reference args */
		ret = tplg_update_string_from_attributes(ref_object, attr->value.string);
		if (ret < 0) {
			SNDERR("Failed to update %s attributes from args\n", object->name);
			return ret;
		}
	}

	return 0;
}

static int tplg_process_child_objects(struct tplg_object *parent)
{
	struct list_head *pos;
	int ret;

	list_for_each(pos, &parent->object_list) {
		struct tplg_object *object = list_entry(pos, struct tplg_object, list);

		ret = tplg_update_attributes_from_parent(object, parent);
		if (ret < 0) {
			SNDERR("failed to update arguments for %s\n", object->name);
			return ret;
		}

		/* update object name after args update */
		tplg_update_object_name_from_args(object);
		snd_strlcpy(object->elem->id, object->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

		/* now update its child objects */
		ret = tplg_process_child_objects(object);
		if (ret < 0) {
			SNDERR("Cannot update child object for %s\n", object->name);
		}
	}
	return 0;
}

static int tplg_copy_attribute(struct tplg_attribute *attr, struct tplg_attribute *ref_attr)
{
	struct list_head *pos1;

	snd_strlcpy(attr->name, ref_attr->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(attr->token_ref, ref_attr->token_ref, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(attr->ref, ref_attr->ref, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	attr->found = ref_attr->found;
	attr->param_type = ref_attr->param_type;
	attr->cfg = ref_attr->cfg;
	attr->type = ref_attr->type;

	/* copy value */
	if (ref_attr->found) {
		switch (ref_attr->type) {
		case SND_CONFIG_TYPE_INTEGER:
			attr->value.integer = ref_attr->value.integer;
			break;
		case SND_CONFIG_TYPE_INTEGER64:
			attr->value.integer64 = ref_attr->value.integer64;
			break;
		case SND_CONFIG_TYPE_STRING:
			snd_strlcpy(attr->value.string, ref_attr->value.string,
				    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			break;
		case SND_CONFIG_TYPE_REAL:
		{
			attr->value.d = ref_attr->value.d;
			break;
		}
		case SND_CONFIG_TYPE_COMPOUND:
			break;
		default:
			SNDERR("Unsupported type %d for attribute %s\n", attr->type, attr->name);
			return -EINVAL;
		}
	}

	/* copy attribute constraints */
	INIT_LIST_HEAD(&attr->constraint.value_list);
	attr->constraint.value_ref = ref_attr->constraint.value_ref;
	list_for_each(pos1, &ref_attr->constraint.value_list) {
		struct tplg_attribute_ref *ref;
		struct tplg_attribute_ref *new_ref = calloc(1, sizeof(*new_ref));

		ref = list_entry(pos1, struct tplg_attribute_ref, list);
		memcpy(new_ref, ref, sizeof(*ref));
		list_add(&new_ref->list, &attr->constraint.value_list);
	}
	attr->constraint.mask = ref_attr->constraint.mask;
	attr->constraint.min = INT_MIN;
	attr->constraint.max = INT_MAX;

	return 0;
}

static int tplg_copy_object(snd_tplg_t *tplg, struct tplg_object *src, struct tplg_object *dest,
			     struct list_head *list)
{
	struct tplg_elem *elem;
	struct list_head *pos;
	int ret;

	if (!src || !dest) {
		SNDERR("Invalid src/dest object\n");
		return -EINVAL;
	}

	dest->num_args = src->num_args;
	snd_strlcpy(dest->name, src->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(dest->class_name, src->class_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	dest->type = src->type;
	dest->cfg = src->cfg;
	INIT_LIST_HEAD(&dest->tuple_set_list);
	INIT_LIST_HEAD(&dest->attribute_list);
	INIT_LIST_HEAD(&dest->object_list);

	/* copy attributes */
	list_for_each(pos, &src->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_attribute *new_attr = calloc(1, sizeof(*attr));

		if (!new_attr)
			return -ENOMEM;

		ret = tplg_copy_attribute(new_attr, attr);
		if (ret < 0) {
			SNDERR("Error copying attribute %s\n", attr->name);
			free(new_attr);
			return -ENOMEM;
		}
		list_add_tail(&new_attr->list, &dest->attribute_list);
	}

	/* TODO: handle other class types */
	switch(src->type) {
	case SND_TPLG_CLASS_TYPE_COMPONENT:
	{
		struct tplg_comp_object *dest_comp_object = &dest->object_type.component;
		struct tplg_comp_object *src_comp_object = &src->object_type.component;

		memcpy(dest_comp_object, src_comp_object, sizeof(*dest_comp_object));
		break;
	}
	default:
		break;
	}

	/* copy its child objects */
	list_for_each(pos, &src->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_child = calloc(1, sizeof(*new_child));

		ret = tplg_copy_object(tplg, child, new_child, &dest->object_list);
		if (ret < 0) {
			SNDERR("error copying child object %s\n", child->name);
			return ret;
		}
	}

	elem = tplg_elem_new_common(tplg, NULL, dest->name, SND_TPLG_TYPE_OBJECT);
	if (!elem)
		return -ENOMEM;
	elem->object = dest;
	dest->elem = elem;

	list_add_tail(&dest->list, list);
	return 0;
}

static int tplg_create_link_elem(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *dai_name, *id;
	struct tplg_attribute *default_hw_cfg;
	struct tplg_dai_object *dai = &object->object_type.dai;
	struct tplg_elem *link_elem, *data_elem;
	struct snd_soc_tplg_link_config *link;
	int ret;

	dai_name = tplg_get_attribute_by_name(&object->attribute_list, "dai_name");
	id = tplg_get_attribute_by_name(&object->attribute_list, "id");
	default_hw_cfg = tplg_get_attribute_by_name(&object->attribute_list, "default_hw_config");

	if (!dai_name || dai_name->type != SND_CONFIG_TYPE_STRING) {
		SNDERR("No DAI name for %s\n", object->name);
		return -EINVAL;
	}

	link_elem = tplg_elem_new_common(tplg, NULL, dai_name->value.string, SND_TPLG_TYPE_BE);
	if (!link_elem)
		return -ENOMEM;
	dai->link_elem = link_elem;

	link = link_elem->link;
	link->size = link_elem->size;
	snd_strlcpy(link->name, link_elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	link->default_hw_config_id = default_hw_cfg->value.integer;
	link->id = id->value.integer;

	/* create data elem for link */
	data_elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_DATA);
        if (!data_elem)
                return -ENOMEM;
	
	ret = tplg_ref_add(link_elem, SND_TPLG_TYPE_DATA, data_elem->id);
	if (ret < 0) {
		SNDERR("failed to add data elem %s to link elem %s\n", data_elem->id,
		       link_elem->id);
		return ret;
	}

	return 0;
}

static int tplg_create_widget_elem(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_comp_object *widget_object = &object->object_type.component;
	struct tplg_elem *widget_elem, *data_elem;
	struct snd_soc_tplg_dapm_widget *widget;
	char *class_name = object->class_name;
	char *elem_name;
	int ret;

	if (strcmp(class_name, "virtual_widget"))
		elem_name = object->name;
	else
		elem_name = strchr(object->name, '.') + 1;

	widget_elem = tplg_elem_new_common(tplg, NULL, elem_name,
						   SND_TPLG_TYPE_DAPM_WIDGET);
	if (!widget_elem)
		return -ENOMEM;

	/* create data elem for w */
	data_elem = tplg_elem_new_common(tplg, NULL, elem_name, SND_TPLG_TYPE_DATA);
        if (!data_elem)
                return -ENOMEM;

	ret = tplg_ref_add(widget_elem, SND_TPLG_TYPE_DATA, data_elem->id);
	if (ret < 0) {
		SNDERR("failed to add data elem %s to widget elem %s\n", data_elem->id,
		       widget_elem->id);
		return ret;
	}

	widget_object->widget_elem = widget_elem;
	widget = widget_elem->widget;
	widget->id = widget_object->widget_id;
	widget->size = widget_elem->size;
	snd_strlcpy(widget->name, widget_elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	return 0;
}

static int tplg_copy_child_objects(snd_tplg_t *tplg, struct tplg_class *class,
				  struct tplg_object *object)
{
	struct list_head *pos, *pos1;
	int ret;

	/* reference objects are not created when the class is created. So create them now. */
	list_for_each(pos, &class->ref_object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_obj = calloc(1, sizeof(*obj));
		struct tplg_elem *class_elem;
		char *class_name = NULL;
		const char *id;

		new_obj->cfg = obj->cfg;

		if (snd_config_get_id(new_obj->cfg, &id) < 0)
			continue;

		/* get class name from parent attribute list */
		list_for_each(pos1, &object->attribute_list) {
			struct tplg_attribute *attr =  list_entry(pos1, struct tplg_attribute, list);

			if (!strcmp(attr->name, id + 1)) {
				class_name = attr->value.string;
				break;
			}
		}

		if (!class_name)
			continue;

		class_elem = tplg_elem_lookup(&tplg->class_list, class_name,
					      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
		if (!class_elem)
			continue;

		ret = tplg_create_child_object(tplg, new_obj->cfg, class_elem, object,
						  &object->object_list);
		if (ret < 0) {
			SNDERR("Error creating object type %s\n", class_elem->id);
			return ret;
		}
	}

	/* copy child objects */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_obj = calloc(1, sizeof(*obj));

		ret = tplg_copy_object(tplg, obj, new_obj, &object->object_list);
		if (ret < 0) {
			free(new_obj);
			return ret;
		}
	}

	return 0;
}

static int tplg_create_dai_object(snd_tplg_t *tplg, struct tplg_class *class,
				  struct tplg_object *object)
{
	struct list_head *pos;
	int ret;

	/* copy class objects into the separate lists  */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);
		struct tplg_object *new_obj = calloc(1, sizeof(*obj));

		switch (obj->type) {
		case SND_TPLG_CLASS_TYPE_BASE:
			if (!strcmp(obj->class_name, "endpoint"))
				break;

			SNDERR("Unexpected child class %s for dai %s\n", obj->class_name,
			       object->name);

			return -EINVAL;
		case SND_TPLG_CLASS_TYPE_COMPONENT:
			break;
		default:
			SNDERR("Unexpected child type %d for %s\n", obj->type, object->name);
			return -EINVAL;
		}

		ret = tplg_copy_object(tplg, obj, new_obj, &object->object_list);
		if (ret < 0) {
			free(new_obj);
			return ret;
		}
	}

	return 0;
}

static int tplg_create_pipeline_object(struct tplg_class *class, struct tplg_object *object)
{
	struct list_head *pos;

	/* check if child objects are of the right type */
	list_for_each(pos, &class->object_list) {
		struct tplg_object *obj = list_entry(pos, struct tplg_object, list);

		switch (obj->type) {
		case SND_TPLG_CLASS_TYPE_BASE:
			if ((!strcmp(obj->class_name, "endpoint")) ||
			   (!strcmp(obj->class_name, "connection")) ||
			   (!strcmp(obj->class_name, "pcm")) ||
			   (!strcmp(obj->class_name, "pcm_caps"))) {
				break;
			}

			SNDERR("Unexpected child class %s for pipeline %s\n", obj->class_name,
			       object->name);
			return -EINVAL;
		case SND_TPLG_CLASS_TYPE_COMPONENT:
			break;
		default:
			SNDERR("Unexpected child object type %d for %s\n", obj->type, object->name);
			return -EINVAL;
		}
	}

	return 0;
}

static int tplg_attribute_check_valid_value(struct tplg_object *object,
					    struct tplg_attribute *attr, char *value)
{
	struct list_head *pos;

	if (list_empty(&attr->constraint.value_list)) {
		snd_strlcpy(attr->value.string, value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		attr->type = SND_CONFIG_TYPE_STRING;
		attr->found = true;
		return 0;
	}

	list_for_each(pos, &attr->constraint.value_list) {
		struct tplg_attribute_ref *v;

		v = list_entry(pos, struct tplg_attribute_ref, list);

		if (!strcmp(value, v->string)) {
			snd_strlcpy(attr->value.string, value, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			attr->type = SND_CONFIG_TYPE_STRING;
			attr->found = true;
			return 0;
		}
	}

	SNDERR("Invalid value '%s' for attribute '%s' in object '%s'\n", value, attr->name,
	       object->name);

	return -EINVAL;
}

/* process object arguments from its constructor */
static int tplg_object_process_args(struct tplg_object *object)
{
	struct list_head *pos;
	char *args;
	int num_arg = 0;
	int ret;

	args = strchr(object->name, '.');

	while (args) {
		char *newargs = strchr(args + 1, '.');
		char *arg;
		int len;
		int i = 0;

		if (!newargs) {
			arg = malloc(strlen(args));
			snd_strlcpy(arg, args + 1, strlen(args));
		} else {
			len = strlen(args) - strlen(newargs);
			arg = malloc(len);
			snd_strlcpy(arg, args + 1, len);
		}

		if (num_arg >= object->num_args)
			break;

		list_for_each(pos, &object->attribute_list) {
			struct tplg_attribute *t_attr;

			t_attr = list_entry(pos, struct tplg_attribute, list);
			if (i == num_arg) {
				/* arguments can either be references from parent or values */
				if (arg[0] == '$') {
					snd_strlcpy(t_attr->ref, arg + 1,
						    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
					snd_strlcpy(t_attr->value.string, arg,
						    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
					t_attr->type = SND_CONFIG_TYPE_STRING;
				} else {
					if (((arg[0] >= '0' && arg[0] <= '9') || arg[0] == '-')) {
						char *end;

						t_attr->value.integer = strtol(arg, &end, 0);
						t_attr->type = SND_CONFIG_TYPE_INTEGER;
					} else {
						ret = tplg_attribute_check_valid_value(object,
										       t_attr, arg);
						if (ret < 0)
							return ret;
					}
				}
				t_attr->found = true;
				break;
			}
			i++;
		}
		args = newargs;
		num_arg++;
	}

	/* Check if all arguments have been provided */
	if (num_arg != object->num_args) {
		SNDERR("Invalid number of arguments %d for object '%s' %d\n", num_arg, object->name, object->num_args);
		return -EINVAL;
	}

	return 0;
}

static int tplg_create_component_object(struct tplg_object *object)
{
	struct tplg_comp_object *comp = &object->object_type.component;
	struct tplg_attribute *widget_type;
	int widget_id;

	widget_type = tplg_get_attribute_by_name(&object->attribute_list, "widget_type");
	if (!widget_type) {
		SNDERR("No widget_type given for %s\n", object->name);
		return -EINVAL;
	}

	widget_id = lookup_widget(widget_type->value.string);

	/* create widget */
	switch (widget_id) {
	case SND_SOC_TPLG_DAPM_PGA:
	case SND_SOC_TPLG_DAPM_BUFFER:
	case SND_SOC_TPLG_DAPM_SCHEDULER:
	case SND_SOC_TPLG_DAPM_EFFECT:
	case SND_SOC_TPLG_DAPM_AIF_IN:
	case SND_SOC_TPLG_DAPM_AIF_OUT:
	case SND_SOC_TPLG_DAPM_DAI_OUT:
	case SND_SOC_TPLG_DAPM_DAI_IN:
	case SND_SOC_TPLG_DAPM_INPUT:
	case SND_SOC_TPLG_DAPM_OUT_DRV:
		comp->widget_id = widget_id;
		break;
	default:
		SNDERR("Invalid widget ID for %s\n", object->name);
		return -EINVAL;
	}

	return 0;
}

struct tplg_object *
tplg_create_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_class *class,
		   struct tplg_object *parent, struct list_head *list)
{
	struct tplg_object *object;
	struct tplg_elem *elem;
	struct list_head *pos;
	const char *name;
	char *object_name;
	int len, ret;

	if (!class) {
		SNDERR("Invalid class elem\n");
		return NULL;
	}

	/* get object arguments */
	if (snd_config_get_id(cfg, &name) < 0) {
		SNDERR("Invalid name for widget\n");
		return NULL;
	}

	len = strlen(name) + strlen(class->name) + 2;
	object_name = calloc(1, len);
	snprintf(object_name, len, "%s.%s", class->name, name);

	elem = tplg_elem_new_common(tplg, NULL, object_name, SND_TPLG_TYPE_OBJECT);
	if (!elem) {
		SNDERR("Failed to create tplg elem for %s\n", object_name);
		return NULL;
	}

	object = calloc(1, sizeof(*object));
	if (!object) {
		return NULL;
	}

	object->cfg = cfg;
	object->elem = elem;
	object->num_args = class->num_args;
	snd_strlcpy(object->name, object_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	snd_strlcpy(object->class_name, class->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	object->type = class->type;
	INIT_LIST_HEAD(&object->tuple_set_list);
	INIT_LIST_HEAD(&object->attribute_list);
	INIT_LIST_HEAD(&object->object_list);
	elem->object = object;
	free(object_name);

	/* copy attributes from class */
	list_for_each(pos, &class->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_attribute *new_attr = calloc(1, sizeof(*attr));

		if (!new_attr)
			return NULL;

		ret = tplg_copy_attribute(new_attr, attr);
		if (ret < 0) {
			SNDERR("Error copying attribute %s\n", attr->name);
			free(new_attr);
			return NULL;
		}
		list_add_tail(&new_attr->list, &object->attribute_list);
	}

	/* process arguments */
	ret = tplg_object_process_args(object);
	if (ret < 0) {
		SNDERR("failed to process arguments for %s\n", object->name);
		return NULL;
	}

	/* process attribute list */
	ret = tplg_process_attributes(cfg, object);
	if (ret < 0) {
		SNDERR("failed to process attributes for %s\n", object->name);
		return NULL;
	}

	/* sanitize objects */
	switch(object->type) {
	case SND_TPLG_CLASS_TYPE_PIPELINE:
		ret = tplg_create_pipeline_object(class, object);
		if (ret < 0) {
			SNDERR("Failed to create pipeline object for %s\n", object->name);
			return NULL;
		}
		break;
	case SND_TPLG_CLASS_TYPE_DAI:
		ret = tplg_create_dai_object(tplg, class, object);
		if (ret < 0) {
			SNDERR("Failed to create DAI object for %s\n", object->name);
			return NULL;
		}
		break;
	case SND_TPLG_CLASS_TYPE_COMPONENT:
		ret = tplg_create_component_object(object);
		if (ret < 0) {
			SNDERR("Failed to create component object for %s\n", object->name);
			return NULL;
		}
		break;
	default:
		break;
	}

	/* update attribute and arguments values from parent args */
	if (parent) {
		ret = tplg_update_attributes_from_parent(object, parent);
		if (ret < 0) {
			SNDERR("failed to update attributes for %s\n", object->name);
			return NULL;
		}
	}

	/* now copy child objects */
	if (object->type != SND_TPLG_CLASS_TYPE_DAI) {
		ret = tplg_copy_child_objects(tplg, class, object);
		if (ret < 0) {
			SNDERR("Failed to create DAI object for %s\n", object->name);
			return NULL;
		}
	}

	/* create child objects */
	ret = tplg_create_child_objects(tplg, cfg, object);
	if (ret < 0) {
		SNDERR("failed to create child objects for %s\n", object->name);
		return NULL;
	}

	/* process child objects and update them with parent args */
	ret = tplg_process_child_objects(object);
	if (ret < 0) {
		SNDERR("failed to create child objects for %s\n", object->name);
		return NULL;
	}

	if (list)
		list_add_tail(&object->list, list);

	return object;
}

static int tplg2_get_bool(struct tplg_attribute *attr)
{
	if (attr->type != SND_CONFIG_TYPE_INTEGER)
		return -EINVAL;

	if (attr->value.integer != 0 && attr->value.integer != 1)
		return -EINVAL;

	return attr->value.integer;
}

static int tplg_get_object_tuple_set(struct tplg_object *object, struct tplg_tuple_set **out,
				     const char *token_ref)
{
	struct list_head *pos, *_pos;
	struct tplg_tuple_set *set;
	const char *type;
	char *tokenref_str;
	int len, set_type;
	int size;

	/* get tuple set type */
	type = strchr(token_ref, '.');
	if (!type) {
		SNDERR("No type given for tuple set: '%s' in object: '%s'\n",
		       token_ref, object->name);
		return -EINVAL;
	}

	set_type = get_tuple_type(type + 1);
	if (set_type < 0) {
		SNDERR("Invalid type for tuple set: '%s' in object: '%s'\n",
		       token_ref, object->name);
		return -EINVAL;
	}

	/* get tuple token ref name */
	len = strlen(token_ref) - strlen(type) + 1;
	tokenref_str = calloc(1, len);
	snd_strlcpy(tokenref_str, token_ref, len);

	/* realloc set if set is found */
	list_for_each_safe(pos, _pos, &object->tuple_set_list) {
		struct tplg_tuple_set *set2;

		set = list_entry(pos, struct tplg_tuple_set, list);

		if (set->type == (unsigned int)set_type && !(strcmp(set->token_ref, tokenref_str))) {

			set->num_tuples++;
			size = sizeof(*set) + set->num_tuples * sizeof(struct tplg_tuple);
			set2 = realloc(set, size);
			if (!set2)
				return -ENOMEM;
			list_del(&set->list);

			set = set2;
			list_add_tail(&set->list, &object->tuple_set_list);
			*out = set;
			return 0;
		}
	}

	/* else create a new set and add it to the object's tuple_set_list */
	size = sizeof(*set) + sizeof(struct tplg_tuple);
	set = calloc(1, size);
	set->num_tuples = 1;
	set->type = set_type;
	snd_strlcpy(set->token_ref, tokenref_str, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	list_add_tail(&set->list, &object->tuple_set_list);
	*out = set;

	return 0;
}

static int tplg_build_object_tuple_set_from_attributes(struct tplg_object *object,
						       struct tplg_attribute *attr)
{
	struct tplg_tuple_set *set;
	struct tplg_tuple *tuple;
	struct list_head *pos;
	int ret;	

	/* get tuple set if it exists already or create one */
	ret = tplg_get_object_tuple_set(object, &set, attr->token_ref);
	if (ret < 0) {
		SNDERR("Invalid tuple set for '%s'\n", object->name);
		return ret;
	}

	/* update set with new tuple */
	tuple = &set->tuple[set->num_tuples - 1];
	snd_strlcpy(tuple->token, attr->name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	switch (set->type) {
	case SND_SOC_TPLG_TUPLE_TYPE_UUID:
	{
		const char *value;

		if (snd_config_get_string(attr->cfg, &value) < 0)
			break;
		if (get_uuid(value, tuple->uuid) < 0) {
			SNDERR("failed to get uuid from string %s\n", value);
			return -EINVAL;
		}
		tplg_dbg("\t\tuuid string %s ", value);
		tplg_dbg("\t\t%s = 0x%x", tuple->token, tuple->uuid);
		break;
	}
	case SND_SOC_TPLG_TUPLE_TYPE_STRING:
		snd_strlcpy(tuple->string, attr->value.string,
			    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		tplg_dbg("\t\t%s = %s", tuple->token, tuple->string);
		break;

	case SND_SOC_TPLG_TUPLE_TYPE_BOOL:
	{
		int ret = tplg2_get_bool(attr);

		if (ret < 0) {
			SNDERR("Invalid value for tuple %s\n", tuple->token);
			return tuple->value;
		}
		tuple->value = ret;
		tplg_dbg("\t\t%s = %d", tuple->token, tuple->value);
		break;
	}
	case SND_SOC_TPLG_TUPLE_TYPE_BYTE:
	case SND_SOC_TPLG_TUPLE_TYPE_SHORT:
	case SND_SOC_TPLG_TUPLE_TYPE_WORD:
	{
		unsigned int tuple_val = 0;

		switch(attr->type) {
		case SND_CONFIG_TYPE_STRING:
			if (!attr->constraint.value_ref) {
				SNDERR("Invalid tuple value type for %s\n", tuple->token);
				return -EINVAL;
			}

			/* convert attribute string values to corresponding integer value */
			list_for_each(pos, &attr->constraint.value_list) {
				struct tplg_attribute_ref *v;

				v = list_entry(pos, struct tplg_attribute_ref, list);
				if (!strcmp(attr->value.string, v->string))
					if (v->value != -EINVAL)
						tuple_val = v->value;
			}
			break;
		case SND_CONFIG_TYPE_INTEGER:
			tuple_val = attr->value.integer;
			break;
		case SND_CONFIG_TYPE_INTEGER64:
			tuple_val = attr->value.integer64;
			break;
		default:
			SNDERR("Invalid value type %d for tuple %s for object %s \n", attr->type,
			       tuple->token, object->name);
			return -EINVAL;
		}

		if ((set->type == SND_SOC_TPLG_TUPLE_TYPE_WORD
				&& tuple_val > UINT_MAX)
			|| (set->type == SND_SOC_TPLG_TUPLE_TYPE_SHORT
				&& tuple_val > USHRT_MAX)
			|| (set->type == SND_SOC_TPLG_TUPLE_TYPE_BYTE
				&& tuple_val > UCHAR_MAX)) {
			SNDERR("tuple %s: invalid value", tuple->token);
			return -EINVAL;
		}

		tuple->value = tuple_val;
		tplg_dbg("\t\t%s = 0x%x", tuple->token, tuple->value);
		break;
	}
	default:
		break;
	}

	return 0;
}

static int tplg_build_object_tuple_sets(struct tplg_object *object)
{
	struct list_head *pos;
	int ret = 0;

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (attr->constraint.mask & TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED) {
			if (attr->found)
				SNDERR("Warning: attibute %s decprecated\n", attr->name);
			continue;
		}

		if (strcmp(attr->token_ref, "")) {
			if (!attr->found)
				continue;

			ret = tplg_build_object_tuple_set_from_attributes(object, attr);
			if (ret < 0)
				return ret;
		}
	}

	return ret;
}

int tplg_build_private_data(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_private *priv;
	struct tplg_elem *data_elem;
	struct list_head *pos;
	int ret;

	/* build tuple sets for object */
	ret = tplg_build_object_tuple_sets(object);
	if (ret < 0)
		return ret;

	data_elem = tplg_elem_lookup(&tplg->pdata_list, object->name,
				      SND_TPLG_TYPE_DATA, SND_TPLG_INDEX_ALL);
	if(!data_elem)
		return 0;
	
	priv = data_elem->data;

	/* build link private data from tuple sets */
	list_for_each(pos, &object->tuple_set_list) {
		struct tplg_tuple_set *set = list_entry(pos, struct tplg_tuple_set, list);
		struct tplg_elem *token_elem;

		if (!set->token_ref) {
			SNDERR("No valid token ref for tuple set type %d\n", set->type);
			return -EINVAL;
		}

		/* get reference token elem */
		token_elem = tplg_elem_lookup(&tplg->token_list, set->token_ref,
					      SND_TPLG_TYPE_TOKEN, SND_TPLG_INDEX_ALL);
		if (!token_elem) {
			SNDERR("No valid tokens for ref %s\n", set->token_ref);
			return -EINVAL;
		}

		ret = scan_tuple_set(data_elem, set, token_elem->tokens, priv ? priv->size : 0);
		if (ret < 0)
			return ret;

		/* priv gets modified while scanning new sets */
		priv = data_elem->data;
	}

	tplg_dbg("Object %s built", object->name);

	return 0;
}

static int tplg_build_dai_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_dai_object *dai = &object->object_type.dai;
	struct snd_soc_tplg_link_config *link;
	struct tplg_elem *l_elem;
	struct list_head *pos, *_pos;
	int i = 0;
	int ret;

	ret = tplg_create_link_elem(tplg, object);
	if (ret < 0) {
		SNDERR("Failed to create widget elem for object\n", object->name);
		return ret;
	}
	l_elem = dai->link_elem;
	link = l_elem->link;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct list_head *pos1;

		if (!strcmp(child->class_name, "hw_config")) {
			struct tplg_attribute *id;
			struct snd_soc_tplg_hw_config *hw_cfg = &link->hw_config[i++];

			/* set hw_config ID */
			id = tplg_get_attribute_by_name(&child->attribute_list, "id");
			if (!id || id->type != SND_CONFIG_TYPE_INTEGER) {
				SNDERR("No ID for hw_config %s\n", child->name);
				return -EINVAL;
			}
			hw_cfg->id = id->value.integer;

			/* parse hw_config params from attributes */
			list_for_each(pos1, &child->attribute_list) {
				struct tplg_attribute *attr;

				attr = list_entry(pos1, struct tplg_attribute, list);
				if (!attr->cfg)
					continue;

				ret = tplg_set_hw_config_param(attr->cfg, hw_cfg);
				if (ret < 0) {
					SNDERR("Error parsing hw_config for object %s\n",
					       object->name);
					return ret;
				}
			}
			tplg_dbg("HW Config: %d", hw_cfg->id);
		}

		if (!strcmp(child->class_name, "pdm_config")) {
			/* build tuple sets for pdm_config object */
			ret = tplg_build_object_tuple_sets(child);
			if (ret < 0)
				return ret;

			list_for_each_safe(pos1, _pos, &child->tuple_set_list) {
				struct tplg_tuple_set *set;
				set = list_entry(pos1, struct tplg_tuple_set, list);
				list_del(&set->list);
				list_add_tail(&set->list, &object->tuple_set_list);
			}
		}
	}

	/* parse link params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_link_param(tplg, attr->cfg, link, NULL);
		if (ret < 0) {
			SNDERR("Error parsing hw_config for object %s\n",
			       object->name);
			return ret;
		}
	}

	link->num_hw_configs = i;
	tplg_dbg("Link elem: %s num_hw_configs: %d", l_elem->id, link->num_hw_configs);

	return tplg_build_private_data(tplg, object);
}

static int tplg2_parse_channel(struct tplg_object *object, struct tplg_elem *mixer_elem)
{
	struct snd_soc_tplg_mixer_control *mc = mixer_elem->mixer_ctrl;
	struct snd_soc_tplg_channel *channel = mc->channel;
	struct list_head *pos;
	char *channel_name = strchr(object->name, '.') + 1;
	int channel_id = lookup_channel(channel_name);

	if (channel_id < 0) {
		SNDERR("invalid channel %d for mixer %s", channel_id, mixer_elem->id);
		return -EINVAL;
	}

	channel += mc->num_channels;

	channel->id = channel_id;
	channel->size = sizeof(*channel);
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "reg"))
			channel->reg = attr->value.integer;


		if (!strcmp(attr->name, "shift"))
			channel->shift = attr->value.integer;
	}

	mc->num_channels++;
	if (mc->num_channels >= SND_SOC_TPLG_MAX_CHAN) {
		SNDERR("Max channels exceeded for %s\n", mixer_elem->id);
		return -EINVAL;
	}

	tplg_dbg("channel: %s id: %d reg:%d shift %d", channel_name, channel->id, channel->reg, channel->shift);

	return 0;
}

static int tplg2_parse_tlv(snd_tplg_t *tplg, struct tplg_object *object,
			    struct tplg_elem *mixer_elem)
{
	struct snd_soc_tplg_ctl_tlv *tplg_tlv;
	struct snd_soc_tplg_tlv_dbscale *scale;
	struct tplg_elem *elem;
	struct list_head *pos;
	int ret;

	/* Just add ref is TLV elem exists already */
	elem = tplg_elem_lookup(&tplg->widget_list, object->name, SND_TPLG_TYPE_TLV,
				SND_TPLG_INDEX_ALL);
	if (elem) {
		tplg_tlv = elem->tlv;
		scale = &tplg_tlv->scale;
		goto ref;
	}

	/* otherwise create new tlv elem */
	elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_TLV);
	if (!elem)
		return -ENOMEM;

	tplg_tlv = elem->tlv;
	tplg_tlv->size = sizeof(struct snd_soc_tplg_ctl_tlv);
	tplg_tlv->type = SNDRV_CTL_TLVT_DB_SCALE;
	scale = &tplg_tlv->scale;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!strcmp(child->class_name, "scale")) {
			list_for_each(pos, &child->attribute_list) {
				struct tplg_attribute *attr;

				attr = list_entry(pos, struct tplg_attribute, list);
				if (!attr->cfg)
					continue;

				ret = tplg_parse_tlv_dbscale_param(attr->cfg, scale);
				if (ret < 0) {
					SNDERR("failed to DBScale for tlv %s", object->name);
					return ret;
				}
			}

			break;
		}
	}
ref:
	tplg_dbg("TLV: %s scale min: %d step %d mute %d", elem->id, scale->min, scale->step, scale->mute);

	ret = tplg_ref_add(mixer_elem, SND_TPLG_TYPE_TLV, elem->id);
	if (ret < 0) {
		SNDERR("failed to add tlv elem %s to mixer elem %s\n",
		       elem->id, mixer_elem->id);
		return ret;
	}

	return 0;
}

static struct tplg_elem *tplg_build_comp_mixer(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_mixer_control *mc;
	struct snd_soc_tplg_ctl_hdr *hdr;
	struct tplg_attribute *name;
	struct tplg_elem *elem;
	struct list_head *pos;
	bool access_set = false, tlv_set = false;
	int j, ret;

	name = tplg_get_attribute_by_name(&object->attribute_list, "name");
	elem = tplg_elem_new_common(tplg, NULL, name->value.string, SND_TPLG_TYPE_MIXER);
	if (!elem)
		return NULL;

	/* init new mixer */
	mc = elem->mixer_ctrl;
	snd_strlcpy(mc->hdr.name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	mc->hdr.type = SND_SOC_TPLG_TYPE_MIXER;
	mc->size = elem->size;
	hdr = &mc->hdr;

	/* set channel reg to default state */
	for (j = 0; j < SND_SOC_TPLG_MAX_CHAN; j++)
		mc->channel[j].reg = -1;

	/* parse some control params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr;

		attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_control_mixer_param(tplg, attr->cfg, mc, elem);
		if (ret < 0) {
			SNDERR("Error parsing hw_config for %s\n", object->name);
			return NULL;
		}

		if (!strcmp(attr->name, "access")) {
			ret = parse_access_values(attr->cfg, &mc->hdr);
			if (ret < 0) {
				SNDERR("Error parsing access attribute for %s\n", object->name);
				return NULL;
			}
			access_set = true;
		}

	}

	/* parse the rest from child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "ops")) {
			ret = tplg_parse_ops(tplg, child->cfg, &mc->hdr);
			if (ret < 0) {
				SNDERR("Error parsing ops for mixer %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "tlv")) {
			ret = tplg2_parse_tlv(tplg, child, elem);
			if (ret < 0) {
				SNDERR("Error parsing tlv for mixer %s\n", object->name);
				return NULL;
			}
			tlv_set = true;
			continue;
		}

		if (!strcmp(child->class_name, "channel")) {
			ret = tplg2_parse_channel(child, elem);
			if (ret < 0) {
				SNDERR("Error parsing channel %d for mixer %s\n", child->name,
				       object->name);
				return NULL;
			}
			continue;
		}
	}
	tplg_dbg("Mixer: %s, num_channels: %d", elem->id, mc->num_channels);
	tplg_dbg("Ops info: %d get: %d put: %d max: %d", hdr->ops.info, hdr->ops.get, hdr->ops.put, mc->max);

	/* set CTL access to default values if none are provided */
	if (!access_set) {
		mc->hdr.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (tlv_set)
			mc->hdr.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	}

	return elem;
}

static struct tplg_elem *tplg_build_comp_bytes(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_bytes_control *be;
	struct snd_soc_tplg_ctl_hdr *hdr;
	struct tplg_elem *elem;
	struct list_head *pos;
	bool access_set = false, tlv_set = false;
	char *name = strchr(object->name, '.') + 1;
	char bytes_data_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int ret;

	snprintf(bytes_data_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%s",
		  "data", name);

	elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_BYTES);
	if (!elem)
		return NULL;

	/* init new byte control */
	be = elem->bytes_ext;
	snd_strlcpy(be->hdr.name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	be->hdr.type = SND_SOC_TPLG_TYPE_BYTES;
	be->size = elem->size;
	hdr = &be->hdr;

	/* parse some control params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr;

		attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_control_bytes_param(tplg, attr->cfg, be, elem);
		if (ret < 0) {
			SNDERR("Error parsing control bytes params for %s\n", object->name);
			return NULL;
		}

		if (!strcmp(attr->name, "access")) {
			ret = parse_access_values(attr->cfg, &be->hdr);
			if (ret < 0) {
				SNDERR("Error parsing access attribute for %s\n", object->name);
				return NULL;
			} else  {
				access_set = true;
			}
		}

	}

	/* parse the rest from child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "ops")) {
			ret = tplg_parse_ops(tplg, child->cfg, &be->hdr);
			if (ret < 0) {
				SNDERR("Error parsing ops for mixer %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "tlv")) {
			ret = tplg2_parse_tlv(tplg, child, elem);
			if (ret < 0) {
				SNDERR("Error parsing tlv for mixer %s\n", object->name);
				return NULL;
			} else {
				tlv_set = true;
			}
			continue;
		}

		if (!strcmp(child->class_name, "extops")) {
			ret = tplg_parse_ext_ops(tplg, child->cfg, &be->hdr);
			if (ret < 0) {
				SNDERR("Error parsing ext ops for bytes %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "data")) {
			struct tplg_attribute *name;

			name = tplg_get_attribute_by_name(&child->attribute_list, "name");
			/* add reference to data elem */
			ret = tplg_ref_add(elem, SND_TPLG_TYPE_DATA, name->value.string);
			if (ret < 0) {
				SNDERR("failed to add data elem %s to byte control %s\n",
				       name->value.string, elem->id);
				return NULL;
			}
		}
	}

	tplg_dbg("Bytes: %s Ops info: %d get: %d put: %d", elem->id, hdr->ops.info, hdr->ops.get,
		 hdr->ops.put);
	tplg_dbg("Ext Ops info: %d get: %d put: %d", be->ext_ops.info, be->ext_ops.get, be->ext_ops.put);

	/* set CTL access to default values if none are provided */
	if (!access_set) {
		be->hdr.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (tlv_set)
			be->hdr.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	}

	return elem;
}

static int tplg2_parse_text_values(snd_config_t *cfg, struct tplg_elem *elem)
{
	struct tplg_texts *texts = elem->texts;
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *value = NULL;
	int j = 0;

	tplg_dbg(" Text Values: %s", elem->id);

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);

		if (j == SND_SOC_TPLG_NUM_TEXTS) {
			tplg_dbg("text string number exceeds %d", j);
			return -ENOMEM;
		}

		/* get value */
		if (snd_config_get_string(n, &value) < 0)
			continue;

		snd_strlcpy(&texts->items[j][0], value,
			    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		tplg_dbg("\t%s", &texts->items[j][0]);

		j++;
	}

	texts->num_items = j;
	return 0;
}

static int tplg_build_text_object(snd_tplg_t *tplg, struct tplg_object *object,
				  struct tplg_elem *m_elem) {
	struct tplg_attribute *values;
	struct tplg_elem *elem;
	int ret;

	values = tplg_get_attribute_by_name(&object->attribute_list, "values");

	if (!values || !values->cfg)
		return 0;

	elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_TEXT);
	if (!elem)
		return -ENOMEM;

	ret = tplg2_parse_text_values(values->cfg, elem);
	if (ret < 0) {
		SNDERR("failed to parse text items");
		return ret;
	}

	ret = tplg_ref_add(m_elem, SND_TPLG_TYPE_TEXT, elem->id);

	tplg_dbg("Text: %s", m_elem->id);

	return 0;
}

static struct tplg_elem *tplg_build_comp_enum(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_enum_control *ec;
	struct snd_soc_tplg_ctl_hdr *hdr;
	struct tplg_elem *elem;
	struct list_head *pos;
	bool access_set = false, tlv_set = false;
	int j, ret;

	elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_ENUM);
	if (!elem)
		return NULL;

	/* init new mixer */
	ec = elem->enum_ctrl;
	snd_strlcpy(ec->hdr.name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	ec->hdr.type = SND_SOC_TPLG_TYPE_ENUM;
	ec->size = elem->size;
	hdr = &ec->hdr;

	/* set channel reg to default state */
	for (j = 0; j < SND_SOC_TPLG_MAX_CHAN; j++)
		ec->channel[j].reg = -1;

	tplg_dbg("Enum: %s", elem->id);

	/* parse some control params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr;

		attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_control_enum_param(tplg, attr->cfg, ec, elem);
		if (ret < 0) {
			SNDERR("Error parsing control enum params for %s\n", object->name);
			return NULL;
		}

		if (!strcmp(attr->name, "access")) {
			ret = parse_access_values(attr->cfg, &ec->hdr);
			if (ret < 0) {
				SNDERR("Error parsing access attribute for %s\n", object->name);
				return NULL;
			} else  {
				access_set = true;
			}
		}

	}

	/* parse the rest from child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "ops")) {
			ret = tplg_parse_ops(tplg, child->cfg, &ec->hdr);
			if (ret < 0) {
				SNDERR("Error parsing ops for enum %s\n", object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "channel")) {
			ret = tplg2_parse_channel(child, elem);
			if (ret < 0) {
				SNDERR("Error parsing channel %d for enum %s\n", child->name,
				       object->name);
				return NULL;
			}
			continue;
		}

		if (!strcmp(child->class_name, "text")) {
			ret = tplg_build_text_object(tplg, child, elem);
			if (ret < 0) {
				SNDERR("Error parsing text for enum %s\n", object->name);
				return NULL;
			} else {
				tlv_set = true;
			}
			continue;
		}
	}


	tplg_dbg("Ops info: %d get: %d put: %d", hdr->ops.info, hdr->ops.get, hdr->ops.put);

	/* set CTL access to default values if none are provided */
	if (!access_set) {
		ec->hdr.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (tlv_set)
			ec->hdr.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	}

	return elem;
}

static int tplg_build_comp_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *pipeline_id;
	struct snd_soc_tplg_dapm_widget *widget;
	struct tplg_comp_object *comp = &object->object_type.component;
	struct tplg_elem *w_elem;
	struct list_head *pos;
	int ret;

	ret = tplg_create_widget_elem(tplg, object);
	if (ret < 0) {
		SNDERR("Failed to create widget elem for object %s\n", object->name);
		return ret;
	}
	w_elem = comp->widget_elem;
	widget = w_elem->widget;

	pipeline_id = tplg_get_attribute_by_name(&object->attribute_list, "pipeline_id");
	if (pipeline_id)
		w_elem->index = pipeline_id->value.integer;

	/* parse widget params from attributes */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "stream_name")) {
			snd_strlcpy(widget->sname, attr->value.string,
				    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			continue;
		}

		if (strcmp(attr->ref, "") || !attr->cfg)
			continue;

		ret = tplg_parse_dapm_widget_param(attr->cfg, widget, NULL);
		if (ret < 0) {
			SNDERR("Error parsing hw_config for object %s\n",
			       object->name);
			return ret;
		}
	}



	/* build controls */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_elem *elem;
		char *class_name = child->class_name;

		if (!strcmp(class_name, "mixer")) {
			struct tplg_attribute *name_attr;
			char *name;

			name_attr = tplg_get_attribute_by_name(&child->attribute_list, "name");
			name = name_attr->value.string;
			if (name[0] == '$')
				continue;
			/*
			 * volume component has 2 mixers defined but not all volume components
			 * define them. So build only the ones that are actually have properly
			 * defined names.
			 */
			elem = tplg_build_comp_mixer(tplg, child);
			if (!elem) {
				SNDERR("Failed to build mixer control for %s\n", object->name);
				return -EINVAL;
			}

			ret = tplg_ref_add(w_elem, SND_TPLG_TYPE_MIXER, elem->id);
			if (ret < 0) {
				SNDERR("failed to add mixer elem %s to widget elem %s\n",
				       elem->id, w_elem->id);
				return ret;
			}
		}

		if (!strcmp(class_name, "bytes")) {
			elem = tplg_build_comp_bytes(tplg, child);
			if (!elem) {
				SNDERR("Failed to build bytes control for %s\n", object->name);
				return -EINVAL;
			}

			ret = tplg_ref_add(w_elem, SND_TPLG_TYPE_BYTES, elem->id);
			if (ret < 0) {
				SNDERR("failed to add bytes control elem %s to widget elem %s\n",
				       elem->id, w_elem->id);
				return ret;
			}
		}

		if (!strcmp(class_name, "enum")) {
			elem = tplg_build_comp_enum(tplg, child);
			if (!elem) {
				SNDERR("Failed to build enum control for %s\n", object->name);
				return -EINVAL;
			}

			ret = tplg_ref_add(w_elem, SND_TPLG_TYPE_ENUM, elem->id);
			if (ret < 0) {
				SNDERR("failed to add enum elem %s to widget elem %s\n",
				       elem->id, w_elem->id);
				return ret;
			}
		}
	}

	tplg_dbg("Widget: %s id: %d stream_name: %s no_pm: %d",
		 w_elem->id, widget->id, widget->sname, widget->reg);

	return tplg_build_private_data(tplg, object);
}

static int tplg_get_sample_size_from_format(char *format)
{
	if (!strcmp(format, "s32le") || !strcmp(format, "s24le") || !strcmp(format, "float") )
		return 4;

	if (!strcmp(format, "s16le"))
		return 2;

	return -EINVAL;
}

static int tplg_pipeline_update_buffer_size(struct tplg_object *pipe_object,
					    struct tplg_object *object)
{
	struct list_head *pos;
	struct tplg_attribute *size_attribute = NULL;
	char pipeline_format[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int periods = 0;
	int sample_size;
	int channels = 0;
	int frames = 0;
	int rate = 0;
	int schedule_period = 0;

	/* get periods and channels from buffer object */
	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "periods")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				periods = attr->value.integer;
			} else {
				SNDERR("Invalid value for periods for object %s \n", object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "channels")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				channels = attr->value.integer;
			} else {
				SNDERR("Invalid value for channels for object %s \n", pipe_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "size"))
			size_attribute = attr;
	}

	if (!size_attribute) {
		SNDERR("Can't find size attribute for %s \n", object->name);
		return -EINVAL;
	}

	/* get schedule_period, channels, rate and format from pipeline object */
	list_for_each(pos, &pipe_object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "period")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				schedule_period = attr->value.integer;
			} else {
				SNDERR("Invalid value for period for object %s \n", pipe_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "rate")) {
			if (attr->type == SND_CONFIG_TYPE_INTEGER) {
				rate = attr->value.integer;
			} else {
				SNDERR("Invalid value for rate for object %s \n", pipe_object->name);
				return -EINVAL;
			}
		}

		if (!strcmp(attr->name, "format")) {
			if (attr->type == SND_CONFIG_TYPE_STRING) {
				snd_strlcpy(pipeline_format, attr->value.string,
					    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			} else {
				SNDERR("Invalid format for pipeline %s \n", pipe_object->name);
				return -EINVAL;
			}
		}
	}

	sample_size = tplg_get_sample_size_from_format(pipeline_format);
	if (sample_size < 0) {
		SNDERR("Invalid value for sample size for object %s \n", pipe_object->name);
		return sample_size;
	}

	/* compute buffer size */
	frames = (rate * schedule_period)/1000000;
	size_attribute->value.integer = periods * sample_size * channels * frames;
	if (!size_attribute->value.integer) {
		SNDERR("Invalid buffer size %d for %s \n",size_attribute->value.integer,
		       object->name);
		return -EINVAL;
	}

	size_attribute->found = true;

	return 0;
}

static int tplg_build_pipeline_object(struct tplg_object *object)
{
	struct tplg_object *pipe_widget = NULL;
	struct list_head *pos;
	int ret;

	/* get the pipe widget */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_comp_object *child_widget = &child->object_type.component;

		if ((child->type == SND_TPLG_CLASS_TYPE_COMPONENT) &&
		    (child_widget->widget_id == SND_SOC_TPLG_DAPM_SCHEDULER)) {
			pipe_widget = child;
			break;
		}
	}

	if (!pipe_widget) {
		SNDERR("No pipeline widget found for %s\n", object->name);
		return -EINVAL;
	}

	/* update buffer size for all buffers in pipeline */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);
		struct tplg_comp_object *child_widget = &child->object_type.component;

		if ((child->type == SND_TPLG_CLASS_TYPE_COMPONENT) &&
		    (child_widget->widget_id == SND_SOC_TPLG_DAPM_BUFFER)) {
			ret = tplg_pipeline_update_buffer_size(object, child);
			if (ret < 0) {
				SNDERR("Error updating buffer size for %s\n", object->name);
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int tplg_build_dapm_route(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_dapm_graph_elem *line;
	struct list_head *pos;
	struct tplg_elem *elem;

	/* create graph elem */
	elem = tplg_elem_new_route(tplg, 0);
	if (!elem)
		return -ENOMEM;

	line = elem->route;

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_elem *w_elem;
		char *dest = NULL;

		if (!strcmp(attr->name, "index")) {
			elem->index = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "source")) {
			dest = line->source;
		}

		/* TODO: check if control is valid */
		if (!strcmp(attr->name, "control"))
			dest = line->control;

		if (!strcmp(attr->name, "sink"))
			dest = line->sink;

		if (!dest || !attr->found)
			continue;

#if 0
		ret = tplg_update_string_from_args(object, attr->value.string);
		if (ret < 0) {
			SNDERR("Failed to update route source %s with args\n",
				attr->value.string);
			return ret;
		}
#endif
		/* check if it is a valid widget */
		w_elem = tplg_elem_lookup(&tplg->widget_list, attr->value.string,
				      SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
		if (!w_elem) {
			SNDERR("No widget elem %s found for route %s\n",
			       attr->value.string, object->name);
			return -EINVAL;
		}

		snd_strlcpy(dest, attr->value.string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	}

	return 0;
}

static int tplg_set_endpoint_dai_pipeline_id(snd_tplg_t *tplg, struct tplg_object *source_ep,
					     struct tplg_object *sink_ep)
{
	struct tplg_attribute *source_ep_widget;
	struct tplg_attribute *sink_id;
	struct tplg_elem *source_elem;

	sink_id = tplg_get_attribute_by_name(&sink_ep->attribute_list, "id");
	source_ep_widget = tplg_get_attribute_by_name(&source_ep->attribute_list, "widget");
	/* lookup widget elem with pipeline name */
	source_elem = tplg_elem_lookup(&tplg->widget_list, source_ep_widget->value.string,
			      SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
	if (!source_elem) {
		SNDERR("No pipeline widget elem %s found\n", source_ep->name);
		return -EINVAL;
	}
	source_elem->index = sink_id->value.integer;

	return 0;
}

static int tplg_set_endpoint_pipeline_sname(snd_tplg_t *tplg, struct tplg_object *source_ep,
					     struct tplg_object *sink_ep)
{
	struct tplg_attribute *sink_type;
	struct tplg_attribute *sink_ep_widget;
	struct tplg_attribute *source_id;
	struct tplg_attribute *sink_id;
	struct snd_soc_tplg_dapm_widget *source_widget;
	struct snd_soc_tplg_dapm_widget *sink_widget;
	struct tplg_elem *source_elem;
	struct tplg_elem *sink_elem;
	char source_pipeline_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char sink_pipeline_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char *pstring = "pipeline";

	sink_type = tplg_get_attribute_by_name(&sink_ep->attribute_list, "class_name");
	source_id = tplg_get_attribute_by_name(&source_ep->attribute_list, "id");
	sink_id = tplg_get_attribute_by_name(&sink_ep->attribute_list, "id");
	snprintf(source_pipeline_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%ld",
		  pstring, source_id->value.integer);

	/* lookup widget elem with pipeline name */
	source_elem = tplg_elem_lookup(&tplg->widget_list, source_pipeline_name,
			      SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
	if (!source_elem) {
		SNDERR("No pipeline widget elem %s found\n", source_pipeline_name);
		return -EINVAL;
	}
	source_widget = source_elem->widget;

	/* pipeline stream name already set */
	if (strcmp(source_widget->sname, ""))
		return 0;

	/* set pipeline stream name to DAI name */
	if (strcmp(sink_type->value.string, "pipeline")) {
		sink_ep_widget = tplg_get_attribute_by_name(&sink_ep->attribute_list, "widget");
		snd_strlcpy(source_widget->sname, sink_ep_widget->value.string,
			    SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		tplg_dbg("Pipeline widget: %s stream_name: %s", source_elem->id, source_widget->sname);
		return 0;
	}
		
	/* if the other end is a pipeline, get stream name from pipeline widget */
	snprintf(sink_pipeline_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s.%ld", pstring,
		 sink_id->value.integer);

	/* lookup widget elem with pipeline name */
	sink_elem = tplg_elem_lookup(&tplg->widget_list, sink_pipeline_name,
			      SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
	if (!sink_elem) {
		SNDERR("No pipeline widget elem %s found\n", sink_pipeline_name);
		return -EINVAL;
	}

	sink_widget = sink_elem->widget;
	snd_strlcpy(source_widget->sname, sink_widget->sname, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	tplg_dbg("Pipeline widget: %s stream_name: %s", source_elem->id, source_widget->sname);
	return 0;
}

static int tplg_build_endpoint_route(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct tplg_attribute *index = tplg_get_attribute_by_name(&object->attribute_list, "index");
	struct snd_soc_tplg_dapm_graph_elem *line;
	struct tplg_elem *elem;
	struct tplg_object *source_ep = NULL, *sink_ep = NULL;
	struct tplg_attribute *source_type, *sink_type;
	struct list_head *pos;
	int ret;

	/* create graph elem */
	elem = tplg_elem_new_route(tplg, 0);
	if (!elem)
		return -ENOMEM;

	line = elem->route;

	/* set elem index */
	elem->index = index->value.integer;

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);
		struct tplg_elem *w_elem, *endpoint_elem;
		struct tplg_object *ep_object;
		struct tplg_attribute *object_attr;
		char *dest = NULL;

		if (strcmp(attr->name, "control") &&
		    strcmp(attr->name, "source") &&
		    strcmp(attr->name, "sink"))
			continue;

		if (!attr->found)
			continue;

		/* look up endpoint object */
		endpoint_elem = tplg_elem_lookup(&tplg->object_list, attr->value.string,
				      SND_TPLG_TYPE_OBJECT, SND_TPLG_INDEX_ALL);
		if (!endpoint_elem) {
			SNDERR("No endpoint elem %s found for route %s\n",
			       attr->value.string, object->name);
			return -EINVAL;
		}

		ep_object = endpoint_elem->object;

		/* get widget attribute for endpoint object */
		object_attr = tplg_get_attribute_by_name(&ep_object->attribute_list, "widget");
		if (!object_attr) {
			SNDERR("No widget attribute for endpoint object name %s\n",
				ep_object->name);
			return -EINVAL;
		}

		/* check if it is a valid widget */
		w_elem = tplg_elem_lookup(&tplg->widget_list, object_attr->value.string,
					  SND_TPLG_TYPE_DAPM_WIDGET, SND_TPLG_INDEX_ALL);
		if (!w_elem) {
			SNDERR("No widget elem %s found for route %s\n",
			       object_attr->value.string, object->name);
			return -EINVAL;
		}

		if (!strcmp(attr->name, "source")) {
			source_ep = ep_object;
			dest = line->source;
		}

		/* TODO: check if control is valid */
		if (!strcmp(attr->name, "control"))
			dest = line->control;

		if (!strcmp(attr->name, "sink")) {
			sink_ep = ep_object;
			dest = line->sink;
		}

		snd_strlcpy(dest, object_attr->value.string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	}

	if (!source_ep || !sink_ep) {
		SNDERR("Connection %s incomplete\n", object->name);
		return -EINVAL;
	}

	/*
	 * set pipeline stream names for pipeline endpoints or pipeline ID for DAI widgets
	 * in case of DAI endpoints
	 */
	source_type = tplg_get_attribute_by_name(&source_ep->attribute_list, "class_name");
	if (!strcmp(source_type->value.string, "pipeline")) {
		ret = tplg_set_endpoint_pipeline_sname(tplg, source_ep, sink_ep);
		if (ret < 0) {
			SNDERR("Failed to set endpoint pipeline stream name for %s\n",
			       source_ep->name);
			return ret;
		}
	} else {
		ret = tplg_set_endpoint_dai_pipeline_id(tplg, source_ep, sink_ep);
		if (ret < 0) {
			SNDERR("Failed to set DAI widget pipeline ID for %s\n",
			       source_ep->name);
			return ret;
		}
	}

	sink_type = tplg_get_attribute_by_name(&sink_ep->attribute_list, "class_name");
	if (!strcmp(sink_type->value.string, "pipeline")) {
		ret = tplg_set_endpoint_pipeline_sname(tplg, sink_ep, source_ep);
		if (ret < 0) {
			SNDERR("Failed to set endpoint pipeline stream name for %s\n",
			       sink_ep->name);
			return ret;
		}
	} else {
	ret = tplg_set_endpoint_dai_pipeline_id(tplg, sink_ep, source_ep);
		if (ret < 0) {
			SNDERR("Failed to set DAI widget pipeline ID for %s\n",
			       sink_ep->name);
			return ret;
		}
	}
	return tplg_build_private_data(tplg, object);
}

static int tplg2_get_unsigned_attribute(struct tplg_attribute *arg, unsigned int *val, int base)
{
	const char *str;
	long lval;
	unsigned long uval;

	if (arg->type == SND_CONFIG_TYPE_INTEGER) {
		lval = arg->value.integer;
		if (lval < 0 && lval >= INT_MIN)
			lval = UINT_MAX + lval + 1;
		if (lval < 0 || lval > UINT_MAX)
			return -ERANGE;
		*val = lval;
		return 0;
	}

	if (arg->type == SND_CONFIG_TYPE_STRING) {
		SNDERR("Invalid type for %s\n", arg->name);
		return -EINVAL;
	}

	str = strdup(arg->value.string);

	uval = strtoul(str, NULL, base);
	if (errno == ERANGE && uval == ULONG_MAX)
		return -ERANGE;
	if (errno && uval == 0)
		return -EINVAL;
	if (uval > UINT_MAX)
		return -ERANGE;
	*val = uval;

	return 0;
}

static struct tplg_elem*
tplg2_lookup_pcm_by_name(snd_tplg_t *tplg, char *pcm_name)
{
	struct snd_soc_tplg_pcm *pcm;
	struct list_head *pos;

	list_for_each(pos, &tplg->pcm_list) {
		struct tplg_elem *elem = list_entry(pos, struct tplg_elem, list);
		pcm = elem->pcm;

		if (!strcmp(pcm->pcm_name, pcm_name)) {
			return elem;
		}
	}

	return NULL;
}

static int tplg_build_pcm_caps_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct snd_soc_tplg_stream_caps *sc;
	struct tplg_elem *elem;
	struct list_head *pos;
	char *pcm_caps_name;
	int ret;

	/* drop the class name from the object name to extract the pcm caps name */
	pcm_caps_name = strchr(object->name, '.') + 1;
	elem = tplg_elem_new_common(tplg, NULL, pcm_caps_name, SND_TPLG_TYPE_STREAM_CAPS);
	if (!elem)
		return -ENOMEM;

	tplg_dbg("PCM caps elem: %s", elem->id);

	sc = elem->stream_caps;
	sc->size = elem->size;
	snd_strlcpy(sc->name, elem->id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, "rate_min")) {
			sc->rate_min = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "rate_max")) {
			sc->rate_max = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "channels_min")) {
			sc->channels_min = attr->value.integer;
			continue;
		}

		if (!strcmp(attr->name, "channels_max")) {
			sc->channels_max = attr->value.integer;
			continue;
		}

		if (!attr->cfg)
			continue;


		ret = tplg_parse_stream_caps_param(attr->cfg, sc);
		if (ret < 0) {
			SNDERR("Failed to parse PCM caps %s\n", object->name);
			return ret;
		}
	}

	return 0;
}

static int tplg_build_pcm_object(snd_tplg_t *tplg, struct tplg_object *object) {

	struct tplg_attribute *pcm_id;
	struct tplg_attribute *name;
	struct tplg_attribute *dir;
	struct snd_soc_tplg_stream_caps *caps;
	struct snd_soc_tplg_pcm *pcm;
	struct tplg_elem *elem;
	struct list_head *pos;
	char *dai_name;
	char *caps_name;
	unsigned int dai_id;
	int ret;

	dir = tplg_get_attribute_by_name(&object->attribute_list, "direction");
	name = tplg_get_attribute_by_name(&object->attribute_list, "pcm_name");
	pcm_id = tplg_get_attribute_by_name(&object->attribute_list, "pcm_id");
	caps_name = strchr(object->name, '.') + 1;
	dai_name = strdup(name->value.string);

	/* check if pcm elem exists already */
	elem = tplg2_lookup_pcm_by_name(tplg, name->value.string);
	if (!elem) {
		elem = tplg_elem_new_common(tplg, NULL, name->value.string, SND_TPLG_TYPE_PCM);
		if (!elem)
			return -ENOMEM;

		pcm = elem->pcm;
		pcm->size = elem->size;

		/* set PCM name */
		snd_strlcpy(pcm->pcm_name, name->value.string, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	} else {
		pcm = elem->pcm;
	}

	ret = tplg2_get_unsigned_attribute(pcm_id, &dai_id, 0);
	if (ret < 0) {
		SNDERR("Invalid value for PCM DAI ID");
		return ret;
	}

	/*TODO: check if pcm_id and dai_id are always the same */
	pcm->pcm_id = dai_id;
	unaligned_put32(&pcm->dai_id, dai_id);

	/* set dai name */
	snprintf(pcm->dai_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s %ld", dai_name,
		 pcm_id->value.integer);
	free(dai_name);

	list_for_each(pos, &object->attribute_list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!attr->cfg)
			continue;

		ret = tplg_parse_pcm_param(tplg, attr->cfg, elem);
		if (ret < 0) {
			SNDERR("Failed to parse PCM %s\n", object->name);
			return -EINVAL;
		}
	}

	caps = pcm->caps;
	if (!strcmp(dir->value.string, "playback")) {
		if (strcmp(caps[SND_SOC_TPLG_STREAM_PLAYBACK].name, "")) {
			SNDERR("PCM Playback capabilities already set for %s\n", object->name);
			return -EINVAL;
		}

		unaligned_put32(&pcm->playback, 1);
		snd_strlcpy(caps[SND_SOC_TPLG_STREAM_PLAYBACK].name, caps_name,
				SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	} else {
		if (strcmp(caps[SND_SOC_TPLG_STREAM_CAPTURE].name, "")) {
			SNDERR("PCM Capture capabilities already set for %s\n", object->name);
			return -EINVAL;
		}

		snd_strlcpy(caps[SND_SOC_TPLG_STREAM_CAPTURE].name, caps_name,
				SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
		unaligned_put32(&pcm->capture, 1);
	}

	tplg_dbg(" PCM: %s ID: %d dai_name: %s", pcm->pcm_name, pcm->dai_id, pcm->dai_name);

	return tplg_build_private_data(tplg, object);
}

static int tplg_build_manifest_object(snd_tplg_t *tplg, struct tplg_object *object) {
	struct snd_soc_tplg_manifest *manifest;
	struct tplg_elem *m_elem;
	struct list_head *pos;
	int ret;

	if (!list_empty(&tplg->manifest_list)) {
		SNDERR("Manifest data already exists");
		return -EINVAL;
	}

	m_elem = tplg_elem_new_common(tplg, NULL, object->name, SND_TPLG_TYPE_MANIFEST);
	if (!m_elem)
		return -ENOMEM;

	manifest = m_elem->manifest;
	manifest->size = m_elem->size;

	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		if (!object->cfg)
			continue;

		if (!strcmp(child->class_name, "data")) {
			struct tplg_attribute *name;

			name = tplg_get_attribute_by_name(&object->attribute_list, "name");

			ret = tplg_ref_add(m_elem, SND_TPLG_TYPE_DATA, name->value.string);
			if (ret < 0) {
				SNDERR("failed to add data elem %s to manifest elem %s\n",
				       name->value.string, m_elem->id);
				return ret;
			}
		}
	}

	tplg_dbg(" Manifest: %s", m_elem->id);
	
	return 0;
}

static int tplg_build_data_object(snd_tplg_t *tplg, struct tplg_object *object) {
	struct tplg_attribute *bytes, *name;
	struct tplg_elem *data_elem;
	int ret;

	name = tplg_get_attribute_by_name(&object->attribute_list, "name");
	if (!name) {
		SNDERR("invalid name for data object: %s", object->name);
		return -EINVAL;
	}

	/* check if data elem exists already */
	data_elem = tplg_elem_lookup(&tplg->widget_list, name->value.string,
				      SND_TPLG_TYPE_DATA, SND_TPLG_INDEX_ALL);
	if (!data_elem) {
		/* create data elem for byte control */
		data_elem = tplg_elem_new_common(tplg, NULL, name->value.string,
						 SND_TPLG_TYPE_DATA);
	        if (!data_elem) {
			SNDERR("failed to create data elem for %s\n", object->name);
	                return -EINVAL;	
		}
	}

	bytes = tplg_get_attribute_by_name(&object->attribute_list, "bytes");

	if (!bytes || !bytes->cfg)
		return 0;

	ret = tplg_parse_data_hex(bytes->cfg, data_elem, 1);
	if (ret < 0)
		SNDERR("failed to parse byte for data: %s", object->name);

	tplg_dbg("data: %s", name->value.string);

	return ret;
}


static int tplg_build_base_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	if (!strcmp(object->class_name, "connection")) {
		struct tplg_attribute *arg;

		arg = tplg_get_attribute_by_name(&object->attribute_list, "type");
		if (!arg) {
			SNDERR("No type for connections %s\n", object->name);
			return -EINVAL;
		}

		if (!strcmp(arg->value.string, "graph"))
			return tplg_build_dapm_route(tplg, object);

		if (!strcmp(arg->value.string, "endpoint"))
			return tplg_build_endpoint_route(tplg, object);

	}

	if (!strcmp(object->class_name, "pcm"))
		return tplg_build_pcm_object(tplg, object);

	if (!strcmp(object->class_name, "pcm_caps"))
		return tplg_build_pcm_caps_object(tplg, object);

	if (!strcmp(object->class_name, "manifest"))
		return tplg_build_manifest_object(tplg, object);

	if (!strcmp(object->class_name, "data"))
		return tplg_build_data_object(tplg, object);

	return 0;
}

static int tplg_build_object(snd_tplg_t *tplg, struct tplg_object *object)
{
	struct list_head *pos;
	int ret;

	switch (object->type) {
	case SND_TPLG_CLASS_TYPE_COMPONENT:
	{
		ret = tplg_build_comp_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build comp object %s\n", object->name);
			return ret;
		}
		break;
	}
	case SND_TPLG_CLASS_TYPE_DAI:
		ret = tplg_build_dai_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build DAI object %s\n", object->name);
			return ret;
		}
		break;

	case SND_TPLG_CLASS_TYPE_PIPELINE:
		ret = tplg_build_pipeline_object(object);
		if (ret < 0) {
			SNDERR("Failed to build DAI object %s\n", object->name);
			return ret;
		}
		break;
	case SND_TPLG_CLASS_TYPE_BASE:
		ret = tplg_build_base_object(tplg, object);
		if (ret < 0) {
			SNDERR("Failed to build object %s\n", object->name);
			return ret;
		}
		break;
	}

	/* build child objects */
	list_for_each(pos, &object->object_list) {
		struct tplg_object *child = list_entry(pos, struct tplg_object, list);

		ret = tplg_build_object(tplg, child);
		if (ret < 0) {
			SNDERR("Failed to build object\n", child->name);
			return ret;
		}
	}

	return 0;
}

int tplg_create_class_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_elem *class_elem,
			     struct list_head *list)
{
	snd_config_iterator_t i, next;
	struct tplg_object *object;
	snd_config_t *n;
	const char *id;

	snd_config_for_each(i, next, cfg) {

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		object = tplg_create_object(tplg, n, class_elem->class, NULL, list);
		if (!object) {
			SNDERR("Failed to create object for class %s\n", class_elem->id);
			return -EINVAL;;
		}
	}

	return 0;
}

static int tplg_define_class_base(snd_tplg_t *tplg, snd_config_t *cfg, int type)
{
	snd_config_iterator_t i, next;
	struct tplg_elem *class_elem;
	struct tplg_class *class;
	struct tplg_elem *elem;
	snd_config_t *n;
	const char *id;
	int ret;

	if (snd_config_get_id(cfg, &id) < 0) {
		SNDERR("Invalid name for class\n");
		return -EINVAL;
	}

	/* check if the class exists already */
	class_elem = tplg_elem_lookup(&tplg->class_list, id,
				      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
	if (class_elem)
		return 0;

	elem = tplg_class_elem(tplg, cfg, type);
	if (!elem)
		return -ENOMEM;

	class = elem->class;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* parse arguments */
		if (!strcmp(id, "@args")) {
			ret = tplg_parse_class_attributes(tplg, n, class,
							  TPLG_CLASS_PARAM_TYPE_ARGUMENT);
			if (ret < 0) {
				SNDERR("failed to parse args for class %s\n", class->name);
				return ret;
			}

			continue;
		}

		/* parse attributes */
		if (!strcmp(id, "DefineAttribute")) {
			ret = tplg_parse_class_attributes(tplg, n, class,
							  TPLG_CLASS_PARAM_TYPE_ATTRIBUTE);
			if (ret < 0) {
				SNDERR("failed to parse attributes for class %s\n", class->name);
				return ret;
			}

			continue;
		}

		/* parse attribute constraints */
		if (!strcmp(id, "attributes")) {
			ret = tplg_parse_class_attribute_categories(n, class);
			if (ret < 0) {
				SNDERR("failed to parse attributes for class %s\n", class->name);
				return ret;
			}
			continue;
		}

		/* parse objects */
		class_elem = tplg_elem_lookup(&tplg->class_list, id,
					      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
		/* create object */
		if (class_elem) {
			ret = tplg_create_class_object(tplg, n, class_elem, &class->object_list);
			if (ret < 0) {
				SNDERR("Cannot create object for class %s\n", id);
				return -EINVAL;
			}

			continue;
		}

		/* class definitions come with default attribute values, process them too */
		ret = tplg_parse_attribute_value(n, &class->attribute_list);
		if (ret < 0) {
			SNDERR("failed to parse attribute value for class %s\n", class->name);
			return -EINVAL;
		}

		/* parse reference objects. These will be created when the type is known */
		if (id[0] == '$') {
			struct tplg_object *ref_object = calloc(1, sizeof(*ref_object));

			if (!ref_object)
				return -ENOMEM;
			ref_object->cfg = n;
			list_add(&ref_object->list, &class->ref_object_list);
		}
	}

	if(!tplg_class_attribute_sanity_check(class))
		return -EINVAL;

	return 0;
}

int tplg_define_class(snd_tplg_t *tplg, snd_config_t *cfg, void *priv ATTRIBUTE_UNUSED)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int class, ret;

	if (snd_config_get_id(cfg, &id) < 0)
		return -EINVAL;

	class = lookup_class_type(id);
	if (class < 0) {
		SNDERR("Invalid class type %s\n", id);
		return -EINVAL;
	}

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		ret = tplg_define_class_base(tplg, n, class);
		if (ret < 0) {
			SNDERR("Failed to create class %s\n", id);
			return ret;
		}
	}

	return 0;
}

int tplg_create_new_object(snd_tplg_t *tplg, snd_config_t *cfg, struct tplg_elem *class_elem,
			   struct tplg_object *parent, struct list_head *list)
{
	snd_config_iterator_t i, next;
	struct tplg_class *class = class_elem->class;
	struct tplg_object *object;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		object = tplg_create_object(tplg, n, class_elem->class, parent, list);
		if (!object) {
			SNDERR("Error creating object for class %s\n", class->name);
			return -EINVAL;
		}

		ret = tplg_build_object(tplg, object);
		if (ret < 0) {
			SNDERR("Error creating object for class %s\n", class->name);
			return -EINVAL;
		}
	}

	return 0;
}

int tplg_create_objects(snd_tplg_t *tplg, snd_config_t *cfg, void *private ATTRIBUTE_UNUSED)
{
	struct tplg_elem *class_elem;
	const char *id;

	if (snd_config_get_id(cfg, &id) < 0)
		return -EINVAL;

	/* look up class elem */
	class_elem = tplg_elem_lookup(&tplg->class_list, id,
				      SND_TPLG_TYPE_CLASS, SND_TPLG_INDEX_ALL);
	if (!class_elem) {
		SNDERR("No class elem found for %s\n", id);
		return -EINVAL;
	}

	return tplg_create_new_object(tplg, cfg, class_elem, NULL, NULL);
}

static void tplg2_free_elem_class(struct tplg_elem *elem)
{
	struct tplg_class *class = elem->class;
	struct list_head *pos, *npos;
        struct tplg_attribute *attr;

	/* free args and attributes. child objects will be freed when tplg->object_list is freed */
	list_for_each_safe(pos, npos, &class->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);
		list_del(&attr->list);
		free(attr);
        }
}

static void tplg2_free_elem_object(struct tplg_elem *elem)
{
	struct tplg_object *object = elem->object;
	struct list_head *pos, *npos;
        struct tplg_attribute *attr;
        struct tplg_tuple_set *set;

	/*
	 * free args, attributes and tuples. Child objects will be freed when
	 * tplg->object_list is freed
	 */
        list_for_each_safe(pos, npos, &object->attribute_list) {
		attr = list_entry(pos, struct tplg_attribute, list);
		list_del(&attr->list);
                free(attr);
        }

	list_for_each_safe(pos, npos, &object->tuple_set_list) {
                set = list_entry(pos, struct tplg_tuple_set, list);
                list_del(&set->list);
                free(set);
        }
}

void tplg2_elem_free(struct tplg_elem *elem)
{
	if (elem->type == SND_TPLG_TYPE_CLASS)
		tplg2_free_elem_class(elem);
	else
		tplg2_free_elem_object(elem);
}

void tplg2_print_elems(snd_tplg_t *tplg)
{
	struct list_head *pos;

	list_for_each(pos, &tplg->class_list) {
		struct tplg_elem *elem = list_entry(pos, struct tplg_elem, list);
		struct tplg_class *class = elem->class;

		//tplg_dbg("class elem %s %s\n", elem->id, class->name);
	}

	list_for_each(pos, &tplg->object_list) {
		struct tplg_elem *elem = list_entry(pos, struct tplg_elem, list);
		struct tplg_object *object = elem->object;

		//tplg_dbg("object elem %s %s\n", elem->id, object->name);
	}	
}
