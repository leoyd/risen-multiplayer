// Routage des captures du héros local (story 2.4) : PlayMotion → LocalMotion, ragdoll →
// LocalRagdoll, noms illisibles écartés.
#include "sync/local_capture.h"
#include "test.h"

#include <cstring>

using namespace mp::sync;
namespace cs = mp::core::session;
namespace proto = mp::protocol;

namespace {

Capture motion_capture(int type, bool desc) {
    Capture c;
    c.kind = CaptureKind::Motion;
    c.type = type;
    c.flag = true;
    c.desc_present = desc;
    const float d00 = 0.2f, d0c = 1.5f;
    std::memcpy(c.desc + 0x00, &d00, 4);
    std::memcpy(c.desc + 0x0C, &d0c, 4);
    return c;
}

}  // namespace

TEST("captures : PlayMotion → LocalMotion avec nom, type, flag et scalaires du descripteur") {
    const auto in = local_input(motion_capture(1, true), "Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400");
    CHECK(in.has_value());
    const auto* lm = in ? std::get_if<cs::LocalMotion>(&*in) : nullptr;
    CHECK(lm != nullptr);
    if (lm != nullptr) {
        CHECK(lm->name == "Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400");
        CHECK_EQ(static_cast<int>(lm->motion.type), 1);
        CHECK_EQ(static_cast<int>(lm->motion.flags), proto::kMotionFlagPlay | proto::kMotionFlagDesc);
        CHECK(lm->motion.d00 == 0.2f && lm->motion.d0c == 1.5f);
        CHECK(lm->motion.net_id == 0 && lm->motion.ani_id == 0 && lm->motion.ms == 0);
    }
}

TEST("captures : sans descripteur → scalaires nuls, encodable") {
    const auto in = local_input(motion_capture(0, false), "Hero_Jump");
    const auto* lm = in ? std::get_if<cs::LocalMotion>(&*in) : nullptr;
    CHECK(lm != nullptr && lm->motion.flags == proto::kMotionFlagPlay && lm->motion.d00 == 0.0f);
    std::vector<std::uint8_t> out;
    if (lm != nullptr) CHECK(proto::encode_reliable(lm->motion, out));
}

TEST("captures : nom illisible, vide, trop long ou type hors 0..7 → rien") {
    CHECK(!local_input(motion_capture(0, true), "").has_value());
    CHECK(!local_input(motion_capture(0, true), "(nom illisible)").has_value());
    CHECK(!local_input(motion_capture(0, true), std::string(256, 'a')).has_value());
    CHECK(local_input(motion_capture(0, true), std::string(255, 'a')).has_value());
    CHECK(!local_input(motion_capture(8, true), "Hero").has_value());
    CHECK(!local_input(motion_capture(-1, true), "Hero").has_value());
}

TEST("captures : ragdoll → rien (la mort part sur Entity::IsDead)") {
    Capture c;
    c.kind = CaptureKind::Ragdoll;
    c.ragdoll_on = true;
    CHECK(!local_input(c, "").has_value());
    c.ragdoll_on = false;
    CHECK(!local_input(c, "Hero").has_value());
}

TEST("captures : réglage de locomotion → LocalParam (type, nature, valeur, durée) ; type hors 0..7 → rien") {
    Capture c;
    c.kind = CaptureKind::Param;
    c.type = 1;
    c.param_kind = 0;
    c.value = 0.75f;
    c.time = 0.3f;
    auto in = local_input(c, "");
    const auto* lp = in ? std::get_if<cs::LocalParam>(&*in) : nullptr;
    CHECK(lp != nullptr && lp->type == 1 && lp->kind == 0 && lp->value == 0.75f && lp->time == 0.3f);
    c.param_kind = 1;
    in = local_input(c, "");
    lp = in ? std::get_if<cs::LocalParam>(&*in) : nullptr;
    CHECK(lp != nullptr && lp->kind == 1);
    c.param_kind = 3;  // arrêt
    c.value = 1.0f;
    in = local_input(c, "");
    lp = in ? std::get_if<cs::LocalParam>(&*in) : nullptr;
    CHECK(lp != nullptr && lp->kind == 3 && lp->value == 1.0f);
    c.type = 8;
    CHECK(!local_input(c, "").has_value());
}
