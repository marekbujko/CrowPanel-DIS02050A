#if HAS_TFT && USE_MCUI

#include "McObserver.h"
#include "McMessages.h"
#include "configuration.h"
#include "crowpanel_backlight.h"

#include "Observer.h"
#include "mesh/MeshTypes.h"
#include "mesh/NodeDB.h"
#include "modules/TextMessageModule.h"

#include <cstring>

namespace mcui {

class TextObserver : public Observer<const meshtastic_MeshPacket *>
{
  public:
    int onNotify(const meshtastic_MeshPacket *mp) override
    {
        if (!mp || !nodeDB) return 0;

        // Cache last-heard RSSI from any packet we can observe, even if it
        // isn't a text message. The Nodes screen reads this.
        if (mp->rx_rssi != 0) node_rssi_set(mp->from, (int16_t)mp->rx_rssi);

        if (mp->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return 0;

        McMessage m = {};
        m.from_node = mp->from;
        m.timestamp = mp->rx_time ? mp->rx_time : (uint32_t)time(nullptr);
        m.snr = mp->rx_snr;
        m.rssi = mp->rx_rssi;
        m.outgoing = false;
        m.delivered = false;
        size_t n = mp->decoded.payload.size;
        if (n >= sizeof(m.text)) n = sizeof(m.text) - 1;
        memcpy(m.text, mp->decoded.payload.bytes, n);
        m.text[n] = '\0';

        NodeNum ours = nodeDB->getNodeNum();
        McConvId id;
        if (mp->to == NODENUM_BROADCAST) {
            id = McConvId::channel(mp->channel);
        } else if (mp->to == ours) {
            // Direct message TO us — conversation is keyed by the sender
            id = McConvId::direct(mp->from);
        } else {
            // Not for us and not broadcast; ignore
            return 0;
        }
        messages_append(id, m);
        LOG_INFO("mcui: rx text %u bytes from 0x%x on %s %u",
                 (unsigned)n, (unsigned)mp->from,
                 id.kind == McConvId::DIRECT ? "node" : "ch",
                 (unsigned)id.value);
        // Wake the screen so the user sees the new message without having
        // to tap first, but do not treat mesh traffic as ongoing user
        // activity or it will keep extending the sleep timer indefinitely.
        backlight_wake_if_off();
        return 0; // let other handlers also run
    }
};

static TextObserver *s_observer = nullptr;
static bool s_attached = false;

// Attach (or retry attaching) the text-message observer to the
// TextMessageModule. The UI task is spawned from tftSetup(), which runs
// BEFORE setupModules() constructs textMessageModule. So on the very first
// call textMessageModule is usually null. We defer: the UI tick calls this
// every pass until it succeeds, then it's a cheap no-op forever.
void observer_init()
{
    if (s_attached) return;
    if (!textMessageModule) {
        // Not ready yet. Caller will try again on the next UI tick.
        return;
    }
    if (!s_observer) s_observer = new TextObserver();
    s_observer->observe(textMessageModule);
    s_attached = true;
    LOG_INFO("mcui: text message observer attached");
}

} // namespace mcui

#endif
