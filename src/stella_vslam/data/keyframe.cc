#include "stella_vslam/data/common.h"
#include "stella_vslam/data/frame.h"
#include "stella_vslam/data/keyframe.h"
#include "stella_vslam/data/landmark.h"
#include "stella_vslam/data/marker.h"
#include "stella_vslam/data/map_database.h"
#include "stella_vslam/data/bow_database.h"
#include "stella_vslam/feature/orb_params.h"
#include "stella_vslam/util/converter.h"

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

namespace stella_vslam {
namespace data {

keyframe::keyframe(unsigned int id, const frame& frm)
    : id_(id), timestamp_(frm.timestamp_),
      camera_(frm.camera_), orb_params_(frm.orb_params_),
      frm_obs_(frm.frm_obs_), markers_2d_(frm.markers_2d_),
      bow_vec_(frm.bow_vec_), bow_feat_vec_(frm.bow_feat_vec_),
      landmarks_(frm.get_landmarks()) {
    // set pose parameters (pose_wc_, trans_wc_) using frm.pose_cw_
    set_pose_cw(frm.get_pose_cw());
}

keyframe::keyframe(const unsigned int id, const double timestamp,
                   const Mat44_t& pose_cw, camera::base* camera,
                   const feature::orb_params* orb_params, const frame_observation& frm_obs,
                   const bow_vector& bow_vec, const bow_feature_vector& bow_feat_vec)
    : id_(id),
      timestamp_(timestamp), camera_(camera),
      orb_params_(orb_params), frm_obs_(frm_obs),
      bow_vec_(bow_vec), bow_feat_vec_(bow_feat_vec),
      landmarks_(std::vector<std::shared_ptr<landmark>>(frm_obs_.num_keypts_, nullptr)) {
    // set pose parameters (pose_wc_, trans_wc_) using pose_cw_
    set_pose_cw(pose_cw);

    // The following process needs to take place:
    //   should set the pointers of landmarks_ using add_landmark()
    //   should set connections using graph_node->update_connections()
    //   should set spanning_parent_ using graph_node->set_spanning_parent()
    //   should set spanning_children_ using graph_node->add_spanning_child()
    //   should set loop_edges_ using graph_node->add_loop_edge()
}

keyframe::~keyframe() {
    SPDLOG_TRACE("keyframe::~keyframe: {}", id_);
}

std::shared_ptr<keyframe> keyframe::make_keyframe(unsigned int id, const frame& frm) {
    auto ptr = std::allocate_shared<keyframe>(Eigen::aligned_allocator<keyframe>(), id, frm);
    // covisibility graph node (connections is not assigned yet)
    ptr->graph_node_ = stella_vslam::make_unique<graph_node>(ptr);
    return ptr;
}

std::shared_ptr<keyframe> keyframe::make_keyframe(
    const unsigned int id, const double timestamp,
    const Mat44_t& pose_cw, camera::base* camera,
    const feature::orb_params* orb_params, const frame_observation& frm_obs,
    const bow_vector& bow_vec, const bow_feature_vector& bow_feat_vec) {
    auto ptr = std::allocate_shared<keyframe>(
        Eigen::aligned_allocator<keyframe>(),
        id, timestamp,
        pose_cw, camera, orb_params,
        frm_obs, bow_vec, bow_feat_vec);
    // covisibility graph node (connections is not assigned yet)
    ptr->graph_node_ = stella_vslam::make_unique<graph_node>(ptr);
    return ptr;
}

nlohmann::json keyframe::to_json() const {
    // extract landmark IDs
    std::vector<int> landmark_ids(landmarks_.size(), -1);
    for (unsigned int i = 0; i < landmark_ids.size(); ++i) {
        if (landmarks_.at(i) && !landmarks_.at(i)->will_be_erased()) {
            landmark_ids.at(i) = landmarks_.at(i)->id_;
        }
    }

    // extract spanning tree parent
    const auto& spanning_parent = graph_node_->get_spanning_parent();

    // extract spanning tree children
    const auto spanning_children = graph_node_->get_spanning_children();
    std::vector<int> spanning_child_ids;
    spanning_child_ids.reserve(spanning_children.size());
    for (const auto& spanning_child : spanning_children) {
        spanning_child_ids.push_back(spanning_child->id_);
    }

    // extract loop edges
    const auto loop_edges = graph_node_->get_loop_edges();
    std::vector<int> loop_edge_ids;
    for (const auto& loop_edge : loop_edges) {
        loop_edge_ids.push_back(loop_edge->id_);
    }

    return {{"ts", timestamp_},
            {"cam", camera_->name_},
            {"orb_params", orb_params_->name_},
            // camera pose
            {"rot_cw", convert_rotation_to_json(pose_cw_.block<3, 3>(0, 0))},
            {"trans_cw", convert_translation_to_json(pose_cw_.block<3, 1>(0, 3))},
            // features and observations
            {"n_keypts", frm_obs_.num_keypts_},
            {"undist_keypts", convert_keypoints_to_json(frm_obs_.undist_keypts_)},
            {"x_rights", frm_obs_.stereo_x_right_},
            {"depths", frm_obs_.depths_},
            {"descs", convert_descriptors_to_json(frm_obs_.descriptors_)},
            {"lm_ids", landmark_ids},
            // graph information
            {"span_parent", spanning_parent ? spanning_parent->id_ : -1},
            {"span_children", spanning_child_ids},
            {"loop_edges", loop_edge_ids}};
}

void keyframe::set_pose_cw(const Mat44_t& pose_cw) {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    pose_cw_ = pose_cw;

    const Mat33_t rot_cw = pose_cw_.block<3, 3>(0, 0);
    const Vec3_t trans_cw = pose_cw_.block<3, 1>(0, 3);
    const Mat33_t rot_wc = rot_cw.transpose();
    trans_wc_ = -rot_wc * trans_cw;

    pose_wc_ = Mat44_t::Identity();
    pose_wc_.block<3, 3>(0, 0) = rot_wc;
    pose_wc_.block<3, 1>(0, 3) = trans_wc_;
}

void keyframe::set_pose_cw(const g2o::SE3Quat& pose_cw) {
    set_pose_cw(util::converter::to_eigen_mat(pose_cw));
}

Mat44_t keyframe::get_pose_cw() const {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    return pose_cw_;
}

Mat44_t keyframe::get_pose_wc() const {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    return pose_wc_;
}

Vec3_t keyframe::get_trans_wc() const {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    return trans_wc_;
}

Mat33_t keyframe::get_rot_cw() const {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    return pose_cw_.block<3, 3>(0, 0);
}

Vec3_t keyframe::get_trans_cw() const {
    std::lock_guard<std::mutex> lock(mtx_pose_);
    return pose_cw_.block<3, 1>(0, 3);
}

bool keyframe::bow_is_available() const {
    return !bow_vec_.empty() && !bow_feat_vec_.empty();
}

void keyframe::compute_bow(bow_vocabulary* bow_vocab) {
    bow_vocabulary_util::compute_bow(bow_vocab, frm_obs_.descriptors_, bow_vec_, bow_feat_vec_);
}

void keyframe::add_landmark(std::shared_ptr<landmark> lm, const unsigned int idx) {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    landmarks_.at(idx) = lm;
}

void keyframe::erase_landmark_with_index(const unsigned int idx) {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    landmarks_.at(idx) = nullptr;
}

void keyframe::erase_landmark(const std::shared_ptr<landmark>& lm) {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    int idx = lm->get_index_in_keyframe(shared_from_this());
    if (0 <= idx) {
        landmarks_.at(static_cast<unsigned int>(idx)) = nullptr;
    }
}

void keyframe::update_landmarks() {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    for (unsigned int idx = 0; idx < landmarks_.size(); ++idx) {
        auto lm = landmarks_.at(idx);
        if (!lm) {
            continue;
        }
        if (lm->will_be_erased()) {
            continue;
        }

        // update connection
        lm->add_observation(shared_from_this(), idx);
        // update geometry
        lm->update_mean_normal_and_obs_scale_variance();
        lm->compute_descriptor();
    }
}

std::vector<std::shared_ptr<landmark>> keyframe::get_landmarks() const {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    return landmarks_;
}

std::set<std::shared_ptr<landmark>> keyframe::get_valid_landmarks() const {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    std::set<std::shared_ptr<landmark>> valid_landmarks;

    for (const auto& lm : landmarks_) {
        if (!lm) {
            continue;
        }
        if (lm->will_be_erased()) {
            continue;
        }

        valid_landmarks.insert(lm);
    }

    return valid_landmarks;
}

unsigned int keyframe::get_num_tracked_landmarks(const unsigned int min_num_obs_thr) const {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    unsigned int num_tracked_lms = 0;

    if (0 < min_num_obs_thr) {
        for (const auto& lm : landmarks_) {
            if (!lm) {
                continue;
            }
            if (lm->will_be_erased()) {
                continue;
            }

            if (min_num_obs_thr <= lm->num_observations()) {
                ++num_tracked_lms;
            }
        }
    }
    else {
        for (const auto& lm : landmarks_) {
            if (!lm) {
                continue;
            }
            if (lm->will_be_erased()) {
                continue;
            }

            ++num_tracked_lms;
        }
    }

    return num_tracked_lms;
}

std::shared_ptr<landmark>& keyframe::get_landmark(const unsigned int idx) {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    return landmarks_.at(idx);
}

std::vector<unsigned int> keyframe::get_keypoints_in_cell(const float ref_x, const float ref_y, const float margin,
                                                          const int min_level, const int max_level) const {
    return data::get_keypoints_in_cell(camera_, frm_obs_, ref_x, ref_y, margin, min_level, max_level);
}

Vec3_t keyframe::triangulate_stereo(const unsigned int idx) const {
    Mat44_t pose_wc;
    {
        std::lock_guard<std::mutex> lock(mtx_pose_);
        pose_wc = pose_wc_;
    }
    return data::triangulate_stereo(camera_, pose_wc.block<3, 3>(0, 0), pose_wc.block<3, 1>(0, 3), frm_obs_, idx);
}

float keyframe::compute_median_depth(const bool abs) const {
    std::vector<std::shared_ptr<landmark>> landmarks;
    Mat44_t pose_cw;
    {
        std::lock_guard<std::mutex> lock1(mtx_observations_);
        std::lock_guard<std::mutex> lock2(mtx_pose_);
        landmarks = landmarks_;
        pose_cw = pose_cw_;
    }

    std::vector<float> depths;
    depths.reserve(frm_obs_.num_keypts_);
    const Vec3_t rot_cw_z_row = pose_cw.block<1, 3>(2, 0);
    const float trans_cw_z = pose_cw(2, 3);

    for (const auto& lm : landmarks) {
        if (!lm) {
            continue;
        }
        const Vec3_t pos_w = lm->get_pos_in_world();
        const auto pos_c_z = rot_cw_z_row.dot(pos_w) + trans_cw_z;
        depths.push_back(abs ? std::abs(pos_c_z) : pos_c_z);
    }

    std::sort(depths.begin(), depths.end());

    return depths.at((depths.size() - 1) / 2);
}

bool keyframe::depth_is_available() const {
    return camera_->setup_type_ != camera::setup_type_t::Monocular;
}

void keyframe::add_marker(const std::shared_ptr<marker>& mkr) {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    markers_[mkr->id_] = mkr;
}

std::vector<std::shared_ptr<marker>> keyframe::get_markers() const {
    std::lock_guard<std::mutex> lock(mtx_observations_);
    std::vector<std::shared_ptr<marker>> markers;
    markers.reserve(markers_.size());
    for (const auto id_marker : markers_) {
        markers.push_back(id_marker.second);
    }
    return markers;
}

void keyframe::set_not_to_be_erased() {
    cannot_be_erased_ = true;
}

void keyframe::set_to_be_erased() {
    if (!graph_node_->has_loop_edge()) {
        cannot_be_erased_ = false;
    }
}

void keyframe::prepare_for_erasing(map_database* map_db, bow_database* bow_db) {
    // cannot erase the origin
    if (*this == *(map_db->origin_keyfrm_)) {
        return;
    }

    // cannot erase if the frag is raised
    if (cannot_be_erased_) {
        return;
    }

    // 1. raise the flag which indicates it has been erased

    SPDLOG_TRACE("keyframe::prepare_for_erasing {}", id_);
    will_be_erased_ = true;

    // 2. remove associations between keypoints and landmarks

    {
        std::lock_guard<std::mutex> lock(mtx_observations_);
        for (const auto& lm : landmarks_) {
            if (!lm) {
                continue;
            }
            if (lm->will_be_erased()) {
                continue;
            }
            lm->erase_observation(map_db, shared_from_this());
            if (!lm->will_be_erased()) {
                lm->compute_descriptor();
                lm->update_mean_normal_and_obs_scale_variance();
            }
        }
    }

    // 3. recover covisibility graph and spanning tree

    // remove covisibility information
    graph_node_->erase_all_connections();
    // recover spanning tree
    graph_node_->recover_spanning_connections();

    // 3. update frame statistics

    map_db->replace_reference_keyframe(shared_from_this(), graph_node_->get_spanning_parent());

    // 4. remove myself from the databased

    map_db->erase_keyframe(shared_from_this());
    bow_db->erase_keyframe(shared_from_this());
}

bool keyframe::will_be_erased() {
    return will_be_erased_;
}

} // namespace data
} // namespace stella_vslam
