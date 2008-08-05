/*
 * Copyright (C) 2007-2008 Nokia Corporation.
 * Copyright (C) 2008 NXP.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 * Contributor: Frederik Vernelen <frederik.vernelen@nxp.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "gstomx_base_filter.h"
#include "gstomx.h"
#include "gstomx_interface.h"

#include <stdlib.h> /* For calloc, free */
#include <string.h> /* For memcpy */

static gboolean share_input_buffer = FALSE;
static gboolean share_output_buffer = FALSE;

static inline gboolean omx_init (GstOmxBaseFilter *self);

enum
{
    ARG_0,
    ARG_COMPONENT_NAME,
    ARG_LIBRARY_NAME,
    ARG_USE_TIMESTAMPS,
};

static GstElementClass *parent_class = NULL;

static void
setup_ports (GstOmxBaseFilter *self)
{
    GOmxCore *core;
    OMX_PARAM_PORTDEFINITIONTYPE *param;

    core = self->gomx;

    param = calloc (1, sizeof (OMX_PARAM_PORTDEFINITIONTYPE));
    param->nSize = sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
    param->nVersion.s.nVersionMajor = 1;
    param->nVersion.s.nVersionMinor = 1;

    /* Input port configuration. */

    param->nPortIndex = 0;
    OMX_GetParameter (core->omx_handle, OMX_IndexParamPortDefinition, param);
    self->in_port = g_omx_core_setup_port (core, param);
    gst_pad_set_element_private (self->sinkpad, self->in_port);

    /* Output port configuration. */

    param->nPortIndex = 1;
    OMX_GetParameter (core->omx_handle, OMX_IndexParamPortDefinition, param);
    self->out_port = g_omx_core_setup_port (core, param);
    gst_pad_set_element_private (self->srcpad, self->out_port);

    free (param);
}

static GstStateChangeReturn
change_state (GstElement *element,
              GstStateChange transition)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (element);

    GST_LOG_OBJECT (self, "begin");

    GST_INFO_OBJECT (self, "changing state %s - %s",
                     gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
                     gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

    switch (transition)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
            if (!self->initialized)
            {
                if (!omx_init (self))
                    return GST_PAD_LINK_REFUSED;

                self->initialized = TRUE;
            }

            g_omx_core_prepare (self->gomx);
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            g_omx_port_finish (self->in_port);
            g_omx_port_finish (self->out_port);
            break;

        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_omx_core_start (self->gomx);
            break;

        default:
            break;
    }

    ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

    if (ret == GST_STATE_CHANGE_FAILURE)
        return ret;

    switch (transition)
    {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            g_omx_port_pause (self->in_port);
            g_omx_port_pause (self->out_port);
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            gst_pad_pause_task (self->srcpad);
            g_omx_core_stop (self->gomx);
            break;

        case GST_STATE_CHANGE_READY_TO_NULL:
            g_omx_core_finish (self->gomx);
            break;

        default:
            break;
    }

    GST_LOG_OBJECT (self, "end");

    return ret;
}

static void
dispose (GObject *obj)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    g_omx_core_free (self->gomx);

    g_free (self->omx_component);
    g_free (self->omx_library);

    G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void
set_property (GObject *obj,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    switch (prop_id)
    {
        case ARG_COMPONENT_NAME:
            g_free (self->omx_component);
            self->omx_component = g_value_dup_string (value);
            break;
        case ARG_LIBRARY_NAME:
            g_free (self->omx_library);
            self->omx_library = g_value_dup_string (value);
            break;
        case ARG_USE_TIMESTAMPS:
            self->use_timestamps = g_value_get_boolean (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
get_property (GObject *obj,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    switch (prop_id)
    {
        case ARG_COMPONENT_NAME:
            g_value_set_string (value, self->omx_component);
            break;
        case ARG_LIBRARY_NAME:
            g_value_set_string (value, self->omx_library);
            break;
        case ARG_USE_TIMESTAMPS:
            g_value_set_boolean (value, self->use_timestamps);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
type_class_init (gpointer g_class,
                 gpointer class_data)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;

    gobject_class = G_OBJECT_CLASS (g_class);
    gstelement_class = GST_ELEMENT_CLASS (g_class);

    parent_class = g_type_class_ref (GST_TYPE_ELEMENT);

    gobject_class->dispose = dispose;
    gstelement_class->change_state = change_state;

    /* Properties stuff */
    {
        gobject_class->set_property = set_property;
        gobject_class->get_property = get_property;

        g_object_class_install_property (gobject_class, ARG_COMPONENT_NAME,
                                         g_param_spec_string ("component-name", "Component name",
                                                              "Name of the OpenMAX IL component to use",
                                                              NULL, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_LIBRARY_NAME,
                                         g_param_spec_string ("library-name", "Library name",
                                                              "Name of the OpenMAX IL implementation library to use",
                                                              NULL, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_USE_TIMESTAMPS,
                                         g_param_spec_boolean ("use-timestamps", "Use timestamps",
                                                               "Whether or not to use timestamps",
                                                               TRUE, G_PARAM_READWRITE));
    }
}

static inline GstFlowReturn
push_buffer (GstOmxBaseFilter *self,
             GstBuffer *buf)
{
    GstFlowReturn ret;

    /** @todo check if tainted */
    GST_LOG_OBJECT (self, "begin");
    ret = gst_pad_push (self->srcpad, buf);
    GST_LOG_OBJECT (self, "end");

    return ret;
}

static void
output_loop (gpointer data)
{
    GstPad *pad;
    GOmxCore *gomx;
    GOmxPort *out_port;
    GstOmxBaseFilter *self;
    GstFlowReturn ret = GST_FLOW_OK;

    pad = data;
    self = GST_OMX_BASE_FILTER (gst_pad_get_parent (pad));
    gomx = self->gomx;

    GST_LOG_OBJECT (self, "begin");

    out_port = self->out_port;

    if (G_LIKELY (out_port->enabled))
    {
        OMX_BUFFERHEADERTYPE *omx_buffer;

        GST_LOG_OBJECT (self, "request buffer");
        omx_buffer = g_omx_port_request_buffer (out_port);

        GST_LOG_OBJECT (self, "omx_buffer: %p", omx_buffer);

        if (G_UNLIKELY (!omx_buffer))
        {
            GST_WARNING_OBJECT (self, "null buffer: leaving");
            goto leave;
        }

        GST_DEBUG_OBJECT (self, "omx_buffer: size=%lu, len=%lu, flags=%lu, offset=%lu, timestamp=%lld",
                          omx_buffer->nAllocLen, omx_buffer->nFilledLen, omx_buffer->nFlags,
                          omx_buffer->nOffset, omx_buffer->nTimeStamp);

        if (G_LIKELY (omx_buffer->nFilledLen > 0))
        {
            GstBuffer *buf;

#if 1
            /** @todo remove this check */
            if (G_LIKELY (self->in_port->enabled))
            {
                GstCaps *caps = NULL;

                caps = gst_pad_get_negotiated_caps (self->srcpad);

                if (!caps)
                {
                    /** @todo We shouldn't be doing this. */
                    GST_WARNING_OBJECT (self, "faking settings changed notification");
                    if (gomx->settings_changed_cb)
                        gomx->settings_changed_cb (gomx);
                }
                else
                {
                    GST_LOG_OBJECT (self, "caps already fixed: %" GST_PTR_FORMAT, caps);
                    gst_caps_unref (caps);
                }
            }
#endif

            /* buf is always null when the output buffer pointer isn't shared. */
            buf = omx_buffer->pAppPrivate;

            if (buf && !(omx_buffer->nFlags & OMX_BUFFERFLAG_EOS))
            {
                GST_BUFFER_SIZE (buf) = omx_buffer->nFilledLen;
                if (self->use_timestamps)
                {
                    GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale_int (omx_buffer->nTimeStamp,
                                                                            GST_SECOND,
                                                                            OMX_TICKS_PER_SECOND);
                }

                omx_buffer->pAppPrivate = NULL;
                omx_buffer->pBuffer = NULL;
                omx_buffer->nFilledLen = 0;

                ret = push_buffer (self, buf);

                gst_buffer_unref (buf);
            }
            else
            {
                /* This is only meant for the first OpenMAX buffers,
                 * which need to be pre-allocated. */
                /* Also for the very last one. */
                gst_pad_alloc_buffer_and_set_caps (self->srcpad,
                                                   GST_BUFFER_OFFSET_NONE,
                                                   omx_buffer->nFilledLen,
                                                   GST_PAD_CAPS (self->srcpad),
                                                   &buf);

                if (G_LIKELY (buf))
                {
                    memcpy (GST_BUFFER_DATA (buf), omx_buffer->pBuffer + omx_buffer->nOffset, omx_buffer->nFilledLen);
                    if (self->use_timestamps)
                    {
                        GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale (omx_buffer->nTimeStamp,
                                                                            GST_SECOND,
                                                                            OMX_TICKS_PER_SECOND);
                    }

                    omx_buffer->nFilledLen = 0;

                    if (share_output_buffer)
                    {
                        GST_WARNING_OBJECT (self, "couldn't zero-copy");
                        g_free (omx_buffer->pBuffer);
                        omx_buffer->pBuffer = NULL;
                    }

                    ret = push_buffer (self, buf);
                }
                else
                {
                    GST_WARNING_OBJECT (self, "couldn't allocate buffer of size %d",
                                        omx_buffer->nFilledLen);
                }
            }
        }
        else
        {
            GST_WARNING_OBJECT (self, "empty buffer");
        }

        if (G_UNLIKELY (ret != GST_FLOW_OK))
            goto leave;

        if (G_UNLIKELY (omx_buffer->nFlags & OMX_BUFFERFLAG_EOS))
        {
            GST_DEBUG_OBJECT (self, "got eos");
            g_omx_core_set_done (gomx);
            goto leave;
        }

        if (share_output_buffer &&
            !omx_buffer->pBuffer &&
            omx_buffer->nOffset == 0)
        {
            GstBuffer *buf;
            GstFlowReturn result;

            GST_LOG_OBJECT (self, "allocate buffer");
            result = gst_pad_alloc_buffer_and_set_caps (self->srcpad,
                                                        GST_BUFFER_OFFSET_NONE,
                                                        omx_buffer->nAllocLen,
                                                        GST_PAD_CAPS (self->srcpad),
                                                        &buf);

            if (G_LIKELY (result == GST_FLOW_OK))
            {
                gst_buffer_ref (buf);
                omx_buffer->pAppPrivate = buf;

                omx_buffer->pBuffer = GST_BUFFER_DATA (buf);
                omx_buffer->nAllocLen = GST_BUFFER_SIZE (buf);
            }
            else
            {
                GST_WARNING_OBJECT (self, "could not pad allocate buffer, using malloc");
                omx_buffer->pBuffer = g_malloc (omx_buffer->nAllocLen);
            }
        }

        if (share_output_buffer &&
            !omx_buffer->pBuffer)
        {
            GST_WARNING_OBJECT (self, "no input buffer to share");
        }

        GST_LOG_OBJECT (self, "release_buffer");
        g_omx_port_release_buffer (out_port, omx_buffer);
    }

    self->last_pad_push_return = ret;

leave:

    if (ret != GST_FLOW_OK)
    {
        GST_INFO_OBJECT (self, "pause task, reason:  %s",
                         gst_flow_get_name (self->last_pad_push_return));
        gst_pad_pause_task (self->srcpad);
    }

    GST_LOG_OBJECT (self, "end");

    gst_object_unref (self);
}

static inline gboolean
omx_init (GstOmxBaseFilter *self)
{
    g_omx_core_init (self->gomx, self->omx_library, self->omx_component);

    if (self->gomx->omx_error)
        return FALSE;

    /** @todo this should probably go after doing preparations. */
    if (self->omx_setup)
    {
        self->omx_setup (self);
    }

    setup_ports (self);

    return TRUE;
}

static GstPadLinkReturn
pad_src_link (GstPad *pad,
              GstPad *peer)
{
    GstOmxBaseFilter *self;
    GstPadLinkReturn result = GST_PAD_LINK_OK;

    if (GST_PAD_LINKFUNC (peer))
    {
        result = GST_PAD_LINKFUNC (peer) (peer, pad);
    }

    if (result != GST_PAD_LINK_OK)
        return result;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    GST_INFO_OBJECT (self, "link");

    if (!self->initialized)
    {
        if (!omx_init (self))
            return GST_PAD_LINK_REFUSED;

        self->initialized = TRUE;
    }

    return GST_PAD_LINK_OK;
}

static GstPadLinkReturn
pad_sink_link (GstPad *pad,
               GstPad *peer)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    GST_INFO_OBJECT (self, "link");

    if (!self->initialized)
    {
        if (!omx_init (self))
            return GST_PAD_LINK_REFUSED;

        self->initialized = TRUE;
    }

    return GST_PAD_LINK_OK;
}

static GstFlowReturn
pad_chain (GstPad *pad,
           GstBuffer *buf)
{
    GOmxCore *gomx;
    GOmxPort *in_port;
    GstOmxBaseFilter *self;
    GstFlowReturn ret = GST_FLOW_OK;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    gomx = self->gomx;

    GST_LOG_OBJECT (self, "begin");
    GST_LOG_OBJECT (self, "gst_buffer: size=%lu", GST_BUFFER_SIZE (buf));

    GST_LOG_OBJECT (self, "state: %d", gomx->omx_state);

    in_port = self->in_port;

    if (G_LIKELY (in_port->enabled))
    {
        guint buffer_offset = 0;

        while (G_LIKELY (buffer_offset < GST_BUFFER_SIZE (buf)))
        {
            OMX_BUFFERHEADERTYPE *omx_buffer;

            if (self->last_pad_push_return != GST_FLOW_OK)
            {
                goto out_flushing;
            }

            GST_LOG_OBJECT (self, "request buffer");
            omx_buffer = g_omx_port_request_buffer (in_port);

            GST_LOG_OBJECT (self, "omx_buffer: %p", omx_buffer);

            if (G_LIKELY (omx_buffer))
            {
                GST_DEBUG_OBJECT (self, "omx_buffer: size=%lu, len=%lu, flags=%lu, offset=%lu, timestamp=%lld",
                                  omx_buffer->nAllocLen, omx_buffer->nFilledLen, omx_buffer->nFlags,
                                  omx_buffer->nOffset, omx_buffer->nTimeStamp);

                if (omx_buffer->nOffset == 0 &&
                    share_input_buffer)
                {
                    {
                        GstBuffer *old_buf;
                        old_buf = omx_buffer->pAppPrivate;

                        if (old_buf)
                        {
                            gst_buffer_unref (old_buf);
                        }
                        else if (omx_buffer->pBuffer)
                        {
                            g_free (omx_buffer->pBuffer);
                        }
                    }

                    omx_buffer->pBuffer = GST_BUFFER_DATA (buf);
                    omx_buffer->nAllocLen = GST_BUFFER_SIZE (buf);
                    omx_buffer->nFilledLen = GST_BUFFER_SIZE (buf);
                    omx_buffer->pAppPrivate = buf;
                }
                else
                {
                    omx_buffer->nFilledLen = MIN (GST_BUFFER_SIZE (buf) - buffer_offset,
                                                  omx_buffer->nAllocLen - omx_buffer->nOffset);
                    memcpy (omx_buffer->pBuffer + omx_buffer->nOffset, GST_BUFFER_DATA (buf) + buffer_offset, omx_buffer->nFilledLen);
                }

                if (self->use_timestamps)
                {
                    omx_buffer->nTimeStamp = gst_util_uint64_scale_int (GST_BUFFER_TIMESTAMP (buf),
                                                                        OMX_TICKS_PER_SECOND,
                                                                        GST_SECOND);
                }

                buffer_offset += omx_buffer->nFilledLen;

                GST_LOG_OBJECT (self, "release_buffer");
                /** @todo untaint buffer */
                g_omx_port_release_buffer (in_port, omx_buffer);
            }
            else
            {
                GST_WARNING_OBJECT (self, "null buffer");
                goto out_flushing;
            }
        }
    }
    else
    {
        GST_WARNING_OBJECT (self, "done");
        ret = GST_FLOW_UNEXPECTED;
    }

    if (!share_input_buffer)
    {
        gst_buffer_unref (buf);
    }

    GST_LOG_OBJECT (self, "end");

    return ret;

    /* special conditions */
out_flushing:
    {
        gst_buffer_unref (buf);
        return self->last_pad_push_return;
    }
}

static gboolean
pad_event (GstPad *pad,
           GstEvent *event)
{
    GstOmxBaseFilter *self;
    GOmxCore *gomx;
    GOmxPort *in_port;
    GOmxPort *out_port;
    gboolean ret;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));
    gomx = self->gomx;
    in_port = self->in_port;
    out_port = self->out_port;

    GST_LOG_OBJECT (self, "begin");

    GST_INFO_OBJECT (self, "event: %s", GST_EVENT_TYPE_NAME (event));

    switch (GST_EVENT_TYPE (event))
    {
        case GST_EVENT_EOS:
            {
                /* send buffer with eos flag */
                /** @todo move to util */
                {
                    OMX_BUFFERHEADERTYPE *omx_buffer;

                    GST_LOG_OBJECT (self, "request buffer");
                    omx_buffer = g_omx_port_request_buffer (in_port);

                    if (G_LIKELY (omx_buffer))
                    {
                        omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

                        GST_LOG_OBJECT (self, "release_buffer");
                        /* foo_buffer_untaint (omx_buffer); */
                        g_omx_port_release_buffer (in_port, omx_buffer);
                    }
                    else
                    {
                        g_omx_core_set_done (gomx);
                    }
                }

                /* Wait for the output port to get the EOS. */
                g_omx_core_wait_for_done (gomx);
            }

            ret = gst_pad_push_event (self->srcpad, event);
            break;

        case GST_EVENT_FLUSH_START:
            /* unlock loops */
            g_omx_port_pause (in_port);
            g_omx_port_pause (out_port);

            gst_pad_pause_task (self->srcpad);

            /* flush all buffers */
            OMX_SendCommand (gomx->omx_handle, OMX_CommandFlush, OMX_ALL, NULL);

            ret = gst_pad_push_event (self->srcpad, event);
            break;

        case GST_EVENT_FLUSH_STOP:
            ret = gst_pad_push_event (self->srcpad, event);
            self->last_pad_push_return = GST_FLOW_OK;

            g_omx_sem_down (gomx->flush_sem);

            gst_pad_start_task (self->srcpad, output_loop, self->srcpad);

            g_omx_port_resume (in_port);
            g_omx_port_resume (out_port);

            break;

        case GST_EVENT_NEWSEGMENT:
            ret = gst_pad_push_event (self->srcpad, event);
            break;

        default:
            ret = gst_pad_push_event (self->srcpad, event);
            break;
    }

    GST_LOG_OBJECT (self, "end");

    return ret;
}

static gboolean
activate_push (GstPad *pad,
               gboolean active)
{
    gboolean result = TRUE;
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (gst_pad_get_parent (pad));

    if (active)
    {
        GST_DEBUG_OBJECT (self, "activate");
        self->last_pad_push_return = GST_FLOW_OK;

        /* we do not start the task yet if the pad is not connected */
        if (gst_pad_is_linked (pad))
        {
            /** @todo link callback function also needed */
            g_omx_port_resume (self->in_port);
            g_omx_port_resume (self->out_port);

            result = gst_pad_start_task (pad, output_loop, pad);
        }
    }
    else
    {
        GST_DEBUG_OBJECT (self, "deactivate");

        /** @todo disable this until we properly reinitialize the buffers. */
#if 0
        /* flush all buffers */
        OMX_SendCommand (self->gomx->omx_handle, OMX_CommandFlush, OMX_ALL, NULL);
#endif

        /* unlock loops */
        g_omx_port_pause (self->in_port);
        g_omx_port_pause (self->out_port);

        /* make sure streaming finishes */
        if (!self->out_port->tunneled)
            result = gst_pad_stop_task (pad);
    }

    gst_object_unref (self);

    return result;
}

static void
type_instance_init (GTypeInstance *instance,
                    gpointer g_class)
{
    GstOmxBaseFilter *self;
    GstElementClass *element_class;

    element_class = GST_ELEMENT_CLASS (g_class);

    self = GST_OMX_BASE_FILTER (instance);

    GST_LOG_OBJECT (self, "begin");

    self->use_timestamps = TRUE;

    /* GOmx */
    {
        GOmxCore *gomx;
        self->gomx = gomx = g_omx_core_new ();
        gomx->client_data = self;
    }

    self->sinkpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template (element_class, "sink"), "sink");

    gst_pad_set_chain_function (self->sinkpad, pad_chain);
    gst_pad_set_event_function (self->sinkpad, pad_event);
    gst_pad_set_link_function (self->sinkpad, pad_sink_link);

    self->srcpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template (element_class, "src"), "src");

    gst_pad_set_activatepush_function (self->srcpad, activate_push);
    gst_pad_set_link_function (self->srcpad, pad_src_link);

    gst_pad_use_fixed_caps (self->srcpad);

    gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
    gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

    self->omx_library = g_strdup (DEFAULT_LIBRARY_NAME);

    GST_LOG_OBJECT (self, "end");
}

static void
omx_interface_init (GstImplementsInterfaceClass *klass)
{
}

static gboolean
interface_supported (GstImplementsInterface *iface,
                     GType type)
{
    g_assert (type == GST_TYPE_OMX);
    return TRUE;
}

static void
interface_init (GstImplementsInterfaceClass *klass)
{
    klass->supported = interface_supported;
}

GType
gst_omx_base_filter_get_type (void)
{
    static GType type = 0;

    if (G_UNLIKELY (type == 0))
    {
        GTypeInfo *type_info;
        GInterfaceInfo *iface_info;
        GInterfaceInfo *omx_info;

        type_info = g_new0 (GTypeInfo, 1);
        type_info->class_size = sizeof (GstOmxBaseFilterClass);
        type_info->class_init = type_class_init;
        type_info->instance_size = sizeof (GstOmxBaseFilter);
        type_info->instance_init = type_instance_init;

        type = g_type_register_static (GST_TYPE_ELEMENT, "GstOmxBaseFilter", type_info, 0);
        g_free (type_info);

        iface_info = g_new0 (GInterfaceInfo, 1);
        iface_info->interface_init = (GInterfaceInitFunc) interface_init;

        g_type_add_interface_static (type, GST_TYPE_IMPLEMENTS_INTERFACE, iface_info);
        g_free (iface_info);

        omx_info = g_new0 (GInterfaceInfo, 1);
        omx_info->interface_init = (GInterfaceInitFunc) omx_interface_init;

        g_type_add_interface_static (type, GST_TYPE_OMX, omx_info);
        g_free (omx_info);
    }

    return type;
}
