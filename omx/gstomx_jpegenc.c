/*
 * Copyright (C) 2007-2008 Nokia Corporation.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "gstomx_jpegenc.h"
#include "gstomx_base_filter.h"
#include "gstomx.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define OMX_COMPONENT_NAME "OMX.st.image_encoder.jpeg"

static GstOmxBaseFilterClass *parent_class = NULL;

static GstCaps *
generate_src_template (void)
{
    GstCaps *caps;

    caps = gst_caps_new_simple ("image/jpeg",
                                "width", GST_TYPE_INT_RANGE, 16, 4096,
                                "height", GST_TYPE_INT_RANGE, 16, 4096,
                                "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
                                NULL);

    return caps;
}

static GstCaps *
generate_sink_template (void)
{
    GstCaps *caps;
    GstStructure *struc;

    caps = gst_caps_new_empty ();

    struc = gst_structure_new ("video/x-raw-yuv",
                               "width", GST_TYPE_INT_RANGE, 16, 4096,
                               "height", GST_TYPE_INT_RANGE, 16, 4096,
                               "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, 30, 1,
                               NULL);

    {
        GValue list = { 0 };
        GValue val = { 0 };

        g_value_init (&list, GST_TYPE_LIST);
        g_value_init (&val, GST_TYPE_FOURCC);

        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('I', '4', '2', '0'));
        gst_value_list_append_value (&list, &val);

        gst_value_set_fourcc (&val, GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'));
        gst_value_list_append_value (&list, &val);

        gst_structure_set_value (struc, "format", &list);

        g_value_unset (&val);
        g_value_unset (&list);
    }

    gst_caps_append_structure (caps, struc);

    return caps;
}

static void
type_base_init (gpointer g_class)
{
    GstElementClass *element_class;

    element_class = GST_ELEMENT_CLASS (g_class);

    {
        GstElementDetails details;

        details.longname = "OpenMAX IL JPEG image encoder";
        details.klass = "Codec/Encoder/Audio";
        details.description = "Encodes image in JPEG format with OpenMAX IL";
        details.author = "Felipe Contreras";

        gst_element_class_set_details (element_class, &details);
    }

    {
        GstPadTemplate *template;

        template = gst_pad_template_new ("src", GST_PAD_SRC,
                                         GST_PAD_ALWAYS,
                                         generate_src_template ());

        gst_element_class_add_pad_template (element_class, template);
    }

    {
        GstPadTemplate *template;

        template = gst_pad_template_new ("sink", GST_PAD_SINK,
                                         GST_PAD_ALWAYS,
                                         generate_sink_template ());

        gst_element_class_add_pad_template (element_class, template);
    }
}

static void
type_class_init (gpointer g_class,
                 gpointer class_data)
{
    GObjectClass *gobject_class;

    gobject_class = G_OBJECT_CLASS (g_class);

    parent_class = g_type_class_ref (GST_OMX_BASE_FILTER_TYPE);
}

static void
settings_changed_cb (GOmxCore *core)
{
    GstOmxBaseFilter *omx_base;
    guint width;
    guint height;

    omx_base = core->client_data;

    GST_DEBUG_OBJECT (omx_base, "settings changed");

    {
        OMX_PARAM_PORTDEFINITIONTYPE *param;

        param = calloc (1, sizeof (OMX_PARAM_PORTDEFINITIONTYPE));

        param->nSize = sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
        param->nVersion.s.nVersionMajor = 1;
        param->nVersion.s.nVersionMinor = 1;

        param->nPortIndex = 1;
        OMX_GetParameter (omx_base->gomx->omx_handle, OMX_IndexParamPortDefinition, param);

        width = param->format.image.nFrameWidth;
        height = param->format.image.nFrameHeight;

        free (param);
    }

    {
        GstCaps *new_caps;

        new_caps = gst_caps_new_simple ("image/jpeg",
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        "framerate", GST_TYPE_FRACTION, 1, 1,
                                        NULL);

        GST_INFO_OBJECT (omx_base, "caps are: %" GST_PTR_FORMAT, new_caps);
        gst_pad_set_caps (omx_base->srcpad, new_caps);
    }
}

static gboolean
sink_setcaps (GstPad *pad,
              GstCaps *caps)
{
    GstStructure *structure;
    GstOmxBaseFilter *omx_base;
    GOmxCore *gomx;
    OMX_COLOR_FORMATTYPE color_format = OMX_COLOR_FormatYUV420Planar;
    gint width = 0;
    gint height = 0;

    omx_base = GST_OMX_BASE_FILTER (GST_PAD_PARENT (pad));
    gomx = (GOmxCore *) omx_base->gomx;

    GST_INFO_OBJECT (omx_base, "setcaps (sink): %" GST_PTR_FORMAT, caps);

    g_return_val_if_fail (gst_caps_get_size (caps) == 1, FALSE);

    structure = gst_caps_get_structure (caps, 0);

    gst_structure_get_int (structure, "width", &width);
    gst_structure_get_int (structure, "height", &height);

    if (strcmp (gst_structure_get_name (structure), "video/x-raw-yuv") == 0)
    {
        guint32 fourcc;

        if (gst_structure_get_fourcc (structure, "format", &fourcc))
        {
            switch (fourcc)
            {
                case GST_MAKE_FOURCC ('I', '4', '2', '0'):
                    color_format = OMX_COLOR_FormatYUV420Planar;
                    break;
                case GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'):
                    color_format = OMX_COLOR_FormatCbYCrY;
                    break;
            }
        }
    }

    {
        OMX_PARAM_PORTDEFINITIONTYPE *param;
        param = calloc (1, sizeof (OMX_PARAM_PORTDEFINITIONTYPE));
        param->nSize = sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
        param->nVersion.s.nVersionMajor = 1;
        param->nVersion.s.nVersionMinor = 1;

        /* Input port configuration. */
        {
            param->nPortIndex = 0;
            OMX_GetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);

            param->format.image.nFrameWidth = width;
            param->format.image.nFrameHeight = height;
            param->format.image.eColorFormat = color_format;

            OMX_SetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);
        }

        free (param);
    }

    return gst_pad_set_caps (pad, caps);
}

static void
omx_setup (GstOmxBaseFilter *omx_base)
{
    GstOmxJpegEnc *self;
    GOmxCore *gomx;

    self = GST_OMX_JPEGENC (omx_base);
    gomx = (GOmxCore *) omx_base->gomx;

    GST_INFO_OBJECT (omx_base, "begin");

    {
        OMX_PARAM_PORTDEFINITIONTYPE *param;
        gint width, height;

        param = calloc (1, sizeof (OMX_PARAM_PORTDEFINITIONTYPE));
        param->nSize = sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
        param->nVersion.s.nVersionMajor = 1;
        param->nVersion.s.nVersionMinor = 1;

        /* Output port configuration. */
        {
            param->nPortIndex = 1;
            OMX_GetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);

            param->format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;

            OMX_SetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);
        }

        /* some workarounds. */
        /* required for TI components. */
#if 1
        /* the component should do this instead */
        {
            OMX_COLOR_FORMATTYPE color_format;

            param->nPortIndex = 0;
            OMX_GetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);

            width = param->format.image.nFrameWidth;
            height = param->format.image.nFrameHeight;
            color_format = param->format.image.eColorFormat;

            /* this is against the standard; nBufferSize is read-only. */
            switch (color_format)
            {
                case OMX_COLOR_FormatYCbYCr:
                case OMX_COLOR_FormatCbYCrY:
                    param->nBufferSize = (GST_ROUND_UP_16 (width) * GST_ROUND_UP_16 (height)) * 2;
#if 0
                    if (param->nBufferSize >= 400)
                        param->nBufferSize = 400;
#endif
                    break;
                case OMX_COLOR_FormatYUV420Planar:
                    param->nBufferSize = (GST_ROUND_UP_16 (width) * GST_ROUND_UP_16 (height)) * 3 / 2;
#if 0
                    if (param->nBufferSize >= 1600)
                        param->nBufferSize = 1600;
#endif
                    break;
                default:
                    break;
            }

            OMX_SetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);
        }

        /* the component should do this instead */
        {
            param->nPortIndex = 1;
            OMX_GetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);

            param->nBufferSize = width * height;

#if 0
            if (qualityfactor < 10)
                param->nBufferSize /= 10;
            else if (qualityfactor < 100)
                param->nBufferSize /= (100 / qualityfactor);
#endif

            param->format.image.nFrameWidth = width;
            param->format.image.nFrameHeight = height;

            OMX_SetParameter (gomx->omx_handle, OMX_IndexParamPortDefinition, param);
        }
#endif

        free (param);
    }

    GST_INFO_OBJECT (omx_base, "end");
}

static void
type_instance_init (GTypeInstance *instance,
                    gpointer g_class)
{
    GstOmxBaseFilter *omx_base;
    GstOmxJpegEnc *self;

    omx_base = GST_OMX_BASE_FILTER (instance);
    self = GST_OMX_JPEGENC (instance);

    omx_base->omx_component = g_strdup (OMX_COMPONENT_NAME);
    omx_base->omx_setup = omx_setup;

    omx_base->gomx->settings_changed_cb = settings_changed_cb;

    gst_pad_set_setcaps_function (omx_base->sinkpad, sink_setcaps);
}

GType
gst_omx_jpegenc_get_type (void)
{
    static GType type = 0;

    if (G_UNLIKELY (type == 0))
    {
        GTypeInfo *type_info;

        type_info = g_new0 (GTypeInfo, 1);
        type_info->class_size = sizeof (GstOmxJpegEncClass);
        type_info->base_init = type_base_init;
        type_info->class_init = type_class_init;
        type_info->instance_size = sizeof (GstOmxJpegEnc);
        type_info->instance_init = type_instance_init;

        type = g_type_register_static (GST_OMX_BASE_FILTER_TYPE, "GstOmxJpegEnc", type_info, 0);

        g_free (type_info);
    }

    return type;
}
