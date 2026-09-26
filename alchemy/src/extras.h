/*
 * Preset extras: the sequencer's own state -- seeds, knobs, locks, drum
 * patterns, progression, tempo, which slots play -- as core/seq.c's flat
 * byte stream (seq_state_write / seq_state_read), riding in the SDK preset
 * blob next to the pager and the settings screen.
 *
 * Every Alchemy firmware in the family (Smack, Mark, Belt, Relay, seq) has
 * its own schema tag here, so a slot written by one of them fails the
 * schema gate in the next.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/surface/serializable.h"
#include "alchemy/control/preset_capacity.h"
#include "seq.h"

/* The pager (3 pages x 6 pots) and the settings screen need a few hundred
 * bytes on top; keep a comfortable margin under the hard slot ceiling. */
static_assert(SEQ_STATE_BYTES + 2048u <= alchemy::kPresetBlobCapacity,
              "sequencer state no longer fits a preset slot");

struct SeqExtras : public alchemy::Serializable
{
    seq_t* seq = nullptr;

    size_t SerializedSize() const override { return SEQ_STATE_BYTES; }

    void Serialize(uint8_t* out) const override { seq_state_write(seq, out); }

    bool Deserialize(const uint8_t* in) override { return seq_state_read(seq, in); }

    /* 'SEQ' + layout version. seq.h's SEQ_STATE_VERSION guards the stream
     * itself; bump the low byte here when the preset payload changes. */
    uint32_t SchemaHash() const override { return 0x53455100u | 0x01u; }
};
