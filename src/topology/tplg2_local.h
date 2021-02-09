/*
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

#include "local.h"
#include "list.h"
#include "bswap.h"
#include "topology.h"

#include <sound/type_compat.h>
#include <sound/asound.h>
#include <sound/asoc.h>
#include <sound/tlv.h>

#define TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY	1 << 0
#define TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE	1 << 1
#define TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED	1 << 2

#define TPLG_ROUTE_NAME_LENGTH	128

struct tplg_connection {
	struct snd_soc_tplg_dapm_graph_elem graph;
	struct list_head list; /* item in pipeline graph_list */
};

struct tplg_endpoint {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char object_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	struct list_head list; /* item in pipeline graph_list */
};

struct tplg_attribute_ref {
	const char *string;
	int value;
	struct list_head list; /* item in attribute constraint value_list */
};

struct attribute_constraint {
	struct list_head value_list; /* list of valid values */
	const char *value_ref;
	int mask;
	int min;
	int max;
};

enum tplg_class_param_type {
	TPLG_CLASS_PARAM_TYPE_ARGUMENT,
	TPLG_CLASS_PARAM_TYPE_ATTRIBUTE,
};

struct tplg_attribute {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	snd_config_type_t type;
	enum tplg_class_param_type param_type;
	char token_ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char value_ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char ref[SNDRV_CTL_ELEM_ID_NAME_MAXLEN]; /* argument reference */
	bool found;
	snd_config_t *cfg;
	struct attribute_constraint constraint;
	struct list_head list; /* item in class/object attribute list */
	union {
		long integer;
		long long integer64;
		double d;
		char string[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	}value;
};

/* topology class definitions */

/* classes and objects use the same types */
#define SND_TPLG_CLASS_TYPE_BASE		0
#define SND_TPLG_CLASS_TYPE_COMPONENT		1
#define SND_TPLG_CLASS_TYPE_PIPELINE		2
#define SND_TPLG_CLASS_TYPE_DAI		3
#define SND_TPLG_CLASS_TYPE_CONTROL		4

struct tplg_class {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int num_args;
	struct list_head attribute_list;
	struct list_head object_list;
	struct list_head ref_object_list; /* for objects that we don't know the final class type yet */
	int type;
};

struct tplg_dai_object {
	struct tplg_elem *link_elem;
	int num_hw_configs;
};

struct tplg_pipeline_object {
	struct tplg_object *pipe_widget_object;
};

struct tplg_comp_object {
	struct tplg_elem *widget_elem;
	int widget_id;
};

struct tplg_object {
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char class_name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int num_args;
	int num_tuple_sets;
	struct list_head attribute_list;
	struct list_head tuple_set_list;
	struct list_head object_list;
	struct tplg_elem *elem;
	snd_config_t *cfg;
	int type;
	struct list_head list; /* item in parent object list */
	union {
		struct tplg_comp_object component;
		struct tplg_dai_object dai;
		struct tplg_pipeline_object pipeline;
	}object_type;
};

int tplg_define_class(snd_tplg_t *tplg, snd_config_t *cfg, void *priv);
int tplg_new_class_pipeline(snd_tplg_t *tplg, snd_config_t *cfg, void *priv);
int tplg_create_objects(snd_tplg_t *tplg, snd_config_t *cfg, void *priv);
void tplg2_elem_free(struct tplg_elem *elem);
