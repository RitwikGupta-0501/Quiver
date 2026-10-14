#include "../include/volumes.hpp"
#include "../include/utils.hpp"
#include <iostream>
#include <algorithm>

VolumeManager::VolumeManager(const std::vector<std::string>& volumes,
                             const std::string& rootfs,
                             StateStore& store,
                             const std::string& container_id)
    : m_volumes{ volumes }
    , m_rootfs{ rootfs }
    , m_store{ &store }
    , m_container_id{ container_id }
{}

void VolumeManager::create_volume(const std::vector<std::string>& volumes, const VolumeType& type){
    if (!m_store) return;
    bool db_write{ false };
    if(type == VolumeType::temporary){
        db_write = false;
    }
    else if(type == VolumeType::permanent){
        db_write = true;
    }
    else{
        Utils::handle_error("Wrong Volume Type");
    }
    if(db_write){
        auto cont_opt = m_store->get_container(m_container_id);
        if (!cont_opt) {
            std::cerr << "Warning: Container not found: " << m_container_id << '\n';
            return;
        }
        for(const std::string& volume_spec : volumes){
            size_t pos{ volume_spec.find(':') };
            if (pos == std::string::npos) {
                std::cerr << "Warning: Volume spec '" << volume_spec << "' is malformed, skipping entry." << '\n';
                continue;
            }

            VolumeMount v;
            v.host_path = volume_spec.substr(0, pos);
            v.container_path = volume_spec.substr(pos + 1);
            cont_opt->volumes.push_back(v);
        }
        m_store->save_container(*cont_opt);
    }
}

void VolumeManager::remove_volume(const std::vector<std::string>& volumes){
    if (!m_store) return;
    auto cont_opt = m_store->get_container(m_container_id);
    if (!cont_opt) return;

    for(const std::string& volume_path : volumes){
        std::erase_if(cont_opt->volumes, [&](const VolumeMount& v) {
            return v.container_path == volume_path || v.host_path == volume_path;
        });
    }
    m_store->save_container(*cont_opt);
}

void VolumeManager::link_volume(const std::vector<std::string>& volumes, const std::vector<std::string>& containers){
    if (!m_store) return;
    if(volumes.size() != containers.size()){
        Utils::handle_error("More containers or volumes provided");
    }
    size_t no{ volumes.size() };
    for(size_t i{0}; i < no; ++i){
        size_t pos{ volumes[i].find(':') };
        if (pos == std::string::npos) {
            std::cerr << "Warning: Volume spec '" << volumes[i] << "' is malformed, skipping link." << '\n';
            continue;
        }

        auto cont_opt = m_store->get_container(containers[i]);
        if (!cont_opt) {
            std::cerr << "Warning: Container not found: " << containers[i] << '\n';
            continue;
        }

        VolumeMount v;
        v.host_path = volumes[i].substr(0, pos);
        v.container_path = volumes[i].substr(pos + 1);
        cont_opt->volumes.push_back(v);
        m_store->save_container(*cont_opt);
    }
}

void VolumeManager::unlink_volume(const std::vector<std::string>& volumes, const std::vector<std::string>& containers){
    if (!m_store) return;
    if(volumes.size() != containers.size()){
        Utils::handle_error("More containers or volumes provided");
    }
    size_t no{ volumes.size() };
    for(size_t i{0}; i < no; ++i){
        auto cont_opt = m_store->get_container(containers[i]);
        if (!cont_opt) continue;

        std::erase_if(cont_opt->volumes, [&](const VolumeMount& v) {
            return v.container_path == volumes[i] || v.host_path == volumes[i];
        });
        m_store->save_container(*cont_opt);
    }
}
