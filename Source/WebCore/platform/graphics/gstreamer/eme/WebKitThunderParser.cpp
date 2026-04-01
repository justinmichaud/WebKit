/* GStreamer Thunder Parser
 *
 * Copyright (C) 2025 Comcast Inc.
 * Copyright (C) 2025 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "WebKitThunderParser.h"

#if ENABLE(ENCRYPTED_MEDIA) && ENABLE(THUNDER) && USE(GSTREAMER)

#include "CDMProxyThunder.h"
#include "GStreamerCommon.h"
#include "GStreamerEMEUtilities.h"
#include <wtf/glib/WTFGType.h>
#include <wtf/text/StringView.h>

GST_DEBUG_CATEGORY(webkitMediaThunderParserDebugCategory);
#define GST_CAT_DEFAULT webkitMediaThunderParserDebugCategory

typedef struct _WebKitMediaThunderParserPrivate {
    GRefPtr<GstElement> decryptor;
    GRefPtr<GstElement> payloader;
    GRefPtr<GstElement> parser;
} WebKitMediaThunderParserPrivate;

typedef struct _WebKitMediaThunderParser {
    GstBin parent;
    WebKitMediaThunderParserPrivate* priv;
} WebKitMediaThunderParser;

typedef struct _WebKitMediaThunderParserClass {
    GstBinClass parentClass;
} WebKitMediaThunderParserClass;

// cencparser is required only for h264 and h265 video streams.
static constexpr std::array<ASCIILiteral, 2> cencparserMediaTypes = { "video/x-h264"_s, "video/x-h265"_s };

using namespace WebCore;

WEBKIT_DEFINE_TYPE(WebKitMediaThunderParser, webkit_media_thunder_parser, GST_TYPE_BIN)

static GstStaticPadTemplate thunderParseSrcTemplate = GST_STATIC_PAD_TEMPLATE("src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS(
        "video/webm; "
        "audio/webm; "
        "video/mp4; "
        "audio/mp4; "
        "audio/mpeg; "
        "audio/x-flac; "
        "audio/x-eac3; "
        "audio/x-ac3; "
        "audio/x-ac4; "
        "video/x-h264; "
        "video/x-h265; "
        "video/x-vp9; video/x-vp8; "
        "video/x-av1; "
        "audio/x-opus; audio/x-vorbis"));

static GRefPtr<GstCaps> createThunderParseSinkPadTemplateCaps()
{
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_new_empty());

    auto& supportedKeySystems = CDMFactoryThunder::singleton().supportedKeySystems();

    if (supportedKeySystems.isEmpty()) {
        GST_WARNING("no supported key systems in Thunder, we won't be able to decrypt anything with the decryptor");
        return caps;
    }

    for (const auto& mediaType : GStreamerEMEUtilities::s_cencEncryptionMediaTypes) {
        gst_caps_append_structure(caps.get(), gst_structure_new_empty(mediaType.characters()));
        gst_caps_append_structure(caps.get(), gst_structure_new("application/x-cenc", "original-media-type", G_TYPE_STRING, mediaType.characters(), nullptr));
    }
    for (const auto& keySystem : supportedKeySystems) {
        for (const auto& mediaType : GStreamerEMEUtilities::s_cencEncryptionMediaTypes) {
            gst_caps_append_structure(caps.get(), gst_structure_new("application/x-cenc", "original-media-type", G_TYPE_STRING,
                mediaType.characters(), "protection-system", G_TYPE_STRING, GStreamerEMEUtilities::keySystemToUuid(keySystem), nullptr));
        }
    }

    if (supportedKeySystems.contains(GStreamerEMEUtilities::s_WidevineKeySystem) || supportedKeySystems.contains(GStreamerEMEUtilities::s_ClearKeyKeySystem)) {
        for (const auto& mediaType : GStreamerEMEUtilities::s_webmEncryptionMediaTypes) {
            gst_caps_append_structure(caps.get(), gst_structure_new_empty(mediaType.characters()));
            gst_caps_append_structure(caps.get(), gst_structure_new("application/x-webm-enc", "original-media-type", G_TYPE_STRING, mediaType.characters(), nullptr));
        }
    }

    return caps;
}

static void webkitMediaThunderParserConstructed(GObject* object)
{
    G_OBJECT_CLASS(webkit_media_thunder_parser_parent_class)->constructed(object);

    auto self = WEBKIT_MEDIA_THUNDER_PARSER(object);
    self->priv->parser = makeGStreamerElement("parsebin"_s, "inner-parser"_s);

    auto factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECRYPTOR, GST_RANK_MARGINAL);
    factories = g_list_sort(factories, [](gconstpointer p1, gconstpointer p2) -> gint {
        GstPluginFeature *f1, *f2;
        const gchar* name;
        f1 = (GstPluginFeature *) p1;
        f2 = (GstPluginFeature *) p2;
        if ((name = gst_plugin_feature_get_name(f1)) && g_str_has_prefix(name, "webkit"))
            return -1;
        if ((name = gst_plugin_feature_get_name(f2)) && g_str_has_prefix(name, "webkit"))
            return 1;
        return gst_plugin_feature_rank_compare_func(p1, p2);
    });
    for (GList* tmp = factories; tmp; tmp = tmp->next) {
        auto factory = GST_ELEMENT_FACTORY_CAST(tmp->data);
        self->priv->decryptor = gst_element_factory_create(factory, nullptr);
        if (self->priv->decryptor) {
            GST_DEBUG_OBJECT(self, "Using decryptor %" GST_PTR_FORMAT, self->priv->decryptor.get());
            break;
        }
    }
    gst_plugin_feature_list_free(factories);

    if (!self->priv->decryptor) [[unlikely]] {
        GST_DEBUG_OBJECT(self, "Unable to find any decryptor, encrypted buffers will be passed-through");
        self->priv->decryptor = gst_element_factory_make("identity", nullptr);
    }

    gst_bin_add_many(GST_BIN_CAST(self), self->priv->decryptor.get(), self->priv->parser.get(), nullptr);
    gst_element_link(self->priv->decryptor.get(), self->priv->parser.get());

    // Try setting up an 'svppay' payloader element.
    // It is not available on all platforms and needed for video streams only.
    // decryptor -> payloader -> parser
    GRefPtr<GstElementFactory> payloaderFactory = adoptGRef(gst_element_factory_find("svppay"));
    if (payloaderFactory) {
        self->priv->payloader = gst_element_factory_make("svppay", nullptr);
    }
    if (self->priv->payloader) {
        auto decryptorSrc = adoptGRef(gst_element_get_static_pad(self->priv->decryptor.get(), "src"));
        gst_pad_add_probe(decryptorSrc.get(), GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, +[](
            GstPad* pad, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {

            if (!(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM))
                return GST_PAD_PROBE_OK;

            GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
            if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
                return GST_PAD_PROBE_OK;

            auto* self = static_cast<WebKitMediaThunderParser*>(userData);
            GST_DEBUG_OBJECT(pad, "Probe triggered on pad: %s", GST_PAD_NAME(pad));

            GstCaps* caps = nullptr;
            gst_event_parse_caps(event, &caps);

            if (!caps)
                return GST_PAD_PROBE_OK;

            GST_DEBUG_OBJECT(pad, "Current caps: %" GST_PTR_FORMAT, caps);
            if (!WebCore::doCapsHaveType(caps, GST_VIDEO_CAPS_TYPE_PREFIX))
                return GST_PAD_PROBE_REMOVE;

            GST_INFO("Attach payloader %s on pad %s for CAPS %" GST_PTR_FORMAT, GST_OBJECT_NAME(self->priv->payloader.get()), GST_PAD_NAME(pad), caps);

            gst_bin_add(GST_BIN_CAST(self), self->priv->payloader.get());
            gst_element_sync_state_with_parent(self->priv->payloader.get());

            // Insert payloader between decryptor src pad and the parser sink pad.
            GRefPtr<GstPad> peerPad = adoptGRef(gst_pad_get_peer(pad));
            GRefPtr<GstPad> payloaderSinkPad = adoptGRef(gst_element_get_static_pad(self->priv->payloader.get(), "sink"));
            GRefPtr<GstPad> payloaderSrcPad = adoptGRef(gst_element_get_static_pad(self->priv->payloader.get(), "src"));
            ASSERT(peerPad);
            ASSERT(payloaderSinkPad);
            ASSERT(payloaderSrcPad);

            GstPadLinkReturn rc;
            if (!gst_pad_unlink(pad, peerPad.get()))
                GST_ERROR("Failed to unlink '%s' src pad", GST_PAD_NAME(pad));
            else if (GST_PAD_LINK_OK != (rc = gst_pad_link_full(pad, payloaderSinkPad.get(), GST_PAD_LINK_CHECK_NOTHING)))
                GST_ERROR("Failed to link pad to payloaderSinkPad, rc = %d", rc);
            else if (GST_PAD_LINK_OK != (rc = gst_pad_link_full(payloaderSrcPad.get(), peerPad.get(), GST_PAD_LINK_CHECK_NOTHING)))
                GST_ERROR("Failed to link payloaderSrcPad to peerPad, rc = %d", rc);

            return GST_PAD_PROBE_REMOVE;
        }, self, nullptr);
    }

    g_signal_connect(self->priv->parser.get(), "autoplug-factories", G_CALLBACK(+[](GstElement*, GstPad*, GstCaps* caps, gpointer userData) -> GValueArray* {
        auto self = WEBKIT_MEDIA_THUNDER_PARSER(userData);
        ALLOW_DEPRECATED_DECLARATIONS_BEGIN;
        GValueArray* result;

        auto factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODABLE, GST_RANK_MARGINAL);
        auto list = gst_element_factory_list_filter(factories, caps, GST_PAD_SINK, gst_caps_is_fixed(caps));
        result = g_value_array_new(g_list_length(list));
        for (GList* tmp = list; tmp; tmp = tmp->next) {
            auto factory = GST_ELEMENT_FACTORY_CAST(tmp->data);
            auto name = StringView::fromLatin1(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE_CAST(factory)));
            if (name == "webkitthunderparser"_s)
                continue;

            auto decryptorFactoryName = StringView::fromLatin1(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE_CAST(gst_element_get_factory(self->priv->decryptor.get()))));
            if (name == decryptorFactoryName)
                continue;

            GValue value = G_VALUE_INIT;
            g_value_init(&value, G_TYPE_OBJECT);
            g_value_set_object(&value, factory);
            g_value_array_append(result, &value);
            g_value_unset(&value);
        }
        gst_plugin_feature_list_free(list);
        gst_plugin_feature_list_free(factories);
        return result;
        ALLOW_DEPRECATED_DECLARATIONS_END;
    }), self);

    g_signal_connect(self->priv->parser.get(), "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer userData) {
        static unsigned counter = 0;
        auto name = makeString("src_"_s, counter);
        counter++;
        gst_element_add_pad(GST_ELEMENT_CAST(userData), gst_ghost_pad_new(name.ascii().data(), pad));
    }), self);

    auto decryptorSinkPad = adoptGRef(gst_element_get_static_pad(self->priv->decryptor.get(), "sink"));
    gst_element_add_pad(GST_ELEMENT_CAST(self), gst_ghost_pad_new("sink", decryptorSinkPad.get()));
    gst_bin_sync_children_states(GST_BIN_CAST(self));
}

static void tryInsertCencparser(WebKitMediaThunderParser* self)
{
    auto cencparserFactory = adoptGRef(gst_element_factory_find("cencparser"));
    if (!cencparserFactory)
        return;

    auto sinkPad = adoptGRef(gst_element_get_static_pad(GST_ELEMENT(self), "sink"));
    auto peerPad = adoptGRef(gst_pad_get_peer(sinkPad.get()));
    if (!peerPad) [[unlikely]] {
        GST_WARNING_OBJECT(self, "Couldn't find peer pad.");
        ASSERT_NOT_REACHED();
        return;
    }

    auto peerCaps = adoptGRef(gst_pad_get_current_caps(peerPad.get()));
    if (!peerCaps) {
        peerCaps = adoptGRef(gst_pad_query_caps(peerPad.get(), nullptr));
        if (!peerCaps) {
            GST_WARNING_OBJECT(self, "Couldn't get caps from peer.");
            return;
        }
    }

    GST_DEBUG_OBJECT(self, "Have type: %" GST_PTR_FORMAT, peerCaps.get());

    bool shouldInsertCencparser = std::any_of(
        cencparserMediaTypes.begin(), cencparserMediaTypes.end(),
        [&peerCaps](const auto& mediaType) {
            return doCapsHaveType(peerCaps.get(), mediaType);
        }
    );
    if (!shouldInsertCencparser) {
        GST_DEBUG_OBJECT(self, "Cencparser is not required.");
        return;
    }

    // Sanity check.
    auto currentSinkPadTarget = adoptGRef(gst_ghost_pad_get_target(GST_GHOST_PAD(sinkPad.get())));
    auto currentSinkPadTargetParent = adoptGRef(gst_pad_get_parent_element(currentSinkPadTarget.get()));
    if (gst_element_get_factory(currentSinkPadTargetParent.get()) == cencparserFactory) {
        GST_DEBUG_OBJECT(self, "Cencparser is already inserted.");
        return;
    }

    // Create and setup cencparser.
    GstElement* cencparser = gst_element_factory_create(cencparserFactory.get(), nullptr);
    if (!cencparser) {
        GST_WARNING_OBJECT(self, "Could not create cencparser.");
        return;
    }
    gst_bin_add(GST_BIN_CAST(self), cencparser);
    gst_base_transform_set_passthrough(
        GST_BASE_TRANSFORM(self->priv->decryptor.get()), !WebCore::areEncryptedCaps(peerCaps.get()));

    // In passthrough mode, decryptor returns an empty caps result for a caps
    // query on sink pad. This is because of lack of clear caps in its sink
    // pad's template. That is the intersection of transformed clear caps with
    // encrypted caps from the template produces an empty result.
    // This empty result causes capsfilter linking to fail inside cencparser.
    // Workaround: intercept cencparser's caps query and append the media types
    // that cencparser expects.
    auto decryptorSinkPad = adoptGRef(gst_element_get_static_pad(self->priv->decryptor.get(), "sink"));
    gst_pad_add_probe(
        decryptorSinkPad.get(),
        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_PULL | GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
        +[](GstPad* pad, GstPadProbeInfo* info, gpointer) -> GstPadProbeReturn {
            if (!GST_IS_QUERY(info->data) || GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) != GST_QUERY_CAPS)
                return GST_PAD_PROBE_OK;

            GRefPtr<GstElement> decryptor = adoptGRef(gst_pad_get_parent_element(pad));
            if (!gst_base_transform_is_passthrough(GST_BASE_TRANSFORM(decryptor.get())))
                return GST_PAD_PROBE_OK;

            GstCaps* queryCaps = nullptr;
            GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
            gst_query_parse_caps_result(query, &queryCaps);
            if (gst_caps_is_empty(queryCaps)) {
                GRefPtr<GstCaps> availableCaps = adoptGRef(gst_caps_new_empty());
                for (const auto& mediaType : cencparserMediaTypes)
                    gst_caps_append_structure(availableCaps.get(), gst_structure_new_empty(mediaType.characters()));

                GstCaps* filter = nullptr;
                gst_query_parse_caps(query, &filter);
                if (filter) {
                    GstCaps* intersection;
                    intersection = gst_caps_intersect_full(filter, availableCaps.get(), GST_CAPS_INTERSECT_FIRST);
                    gst_caps_append(queryCaps, intersection);
                } else
                    gst_caps_append(queryCaps, availableCaps.leakRef());
                GST_DEBUG_OBJECT(pad, "Enriched result caps: %" GST_PTR_FORMAT, queryCaps);
            }
            return GST_PAD_PROBE_OK;
        }, nullptr, nullptr);

    GST_DEBUG_OBJECT(self, "Inserting %s before %s", GST_ELEMENT_NAME(cencparser), GST_ELEMENT_NAME(self->priv->decryptor.get()));
    GRefPtr<GstPad> cencparserSinkPad = adoptGRef(gst_element_get_static_pad(cencparser, "sink"));
    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(sinkPad.get()), cencparserSinkPad.get())) {
        GST_WARNING_OBJECT(self, "Could not change sink pad target.");
        return;
    }
    if (!gst_element_link_pads_full(cencparser, "src", self->priv->decryptor.get(), "sink", GST_PAD_LINK_CHECK_NOTHING)) {
        GST_WARNING_OBJECT(self, "Failed to link %s with %s", GST_ELEMENT_NAME(cencparser), GST_ELEMENT_NAME(self->priv->decryptor.get()));
        return;
    }
    if (!gst_element_sync_state_with_parent(cencparser))
        GST_WARNING_OBJECT(self, "Failed to sync state of %s with parent bin.", GST_ELEMENT_NAME(cencparser));
}

static GstStateChangeReturn webkitMediaThunderParserChangeState(GstElement* element, GstStateChange transition)
{
    WebKitMediaThunderParser* self = WEBKIT_MEDIA_THUNDER_PARSER(element);

    switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY: {
        tryInsertCencparser(self);
        break;
    }
    default:
        break;
    }

    GstStateChangeReturn result = GST_ELEMENT_CLASS(webkit_media_thunder_parser_parent_class)->change_state(element, transition);

    return result;
}

static void webkit_media_thunder_parser_class_init(WebKitMediaThunderParserClass* klass)
{
    GST_DEBUG_CATEGORY_INIT(webkitMediaThunderParserDebugCategory, "webkitthunderparser", 0, "Thunder parser");

    auto objectClass = G_OBJECT_CLASS(klass);
    objectClass->constructed = webkitMediaThunderParserConstructed;

    auto elementClass = GST_ELEMENT_CLASS(klass);
    elementClass->change_state = GST_DEBUG_FUNCPTR(webkitMediaThunderParserChangeState);

    auto padTemplateCaps = createThunderParseSinkPadTemplateCaps();
    gst_element_class_add_pad_template(elementClass, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, padTemplateCaps.get()));
    gst_element_class_add_pad_template(elementClass, gst_static_pad_template_get(&thunderParseSrcTemplate));

    gst_element_class_set_static_metadata(elementClass, "Parse potentially encrypted content", "Codec/Parser/Audio/Video",
        "Parse potentially encrypted content", "Philippe Normand <philn@igalia.com>");
}

#undef GST_CAT_DEFAULT

#endif // ENABLE(ENCRYPTED_MEDIA) && ENABLE(THUNDER) && USE(GSTREAMER)
