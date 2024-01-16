#include "pose_estimator_new.h"
#include "../FRICP-toolkit/registeration.h"


pose_estimator::pose_estimator(){
    allocateMemory();

    nh.param<std::string>("relo/priorDir", priorDir, " ");
    nh.param<std::string>("relo/cloudTopic", cloudTopic, "/cloud_registered");
    nh.param<std::string>("relo/poseTopic", poseTopic, "/Odometry");
    cloudTopic_repub = cloudTopic + "repub";  // FIXME: no use
    poseTopic_repub = poseTopic + "repub";  // FIXME: no use
    nh.param<float>("relo/searchDis", searchDis, 10.0);
    nh.param<int>("relo/searchNum", searchNum, 3);
    nh.param<float>("relo/trustDis", trustDis, 5.0);
    nh.param<int>("relo/regMode", regMode, 5);

    subCloud = nh.subscribe<sensor_msgs::PointCloud2>(cloudTopic, 1, &pose_estimator::cloudCBK, this);
    subPose = nh.subscribe<nav_msgs::Odometry>(poseTopic, 500, &pose_estimator::poseCBK, this);
    pubCloud = nh.advertise<sensor_msgs::PointCloud2>("/cloud", 1);
    pubPose = nh.advertise<nav_msgs::Odometry>("/pose", 1);

    subExternalPose = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/initialpose", 500, &pose_estimator::externalCBK, this);
    pubPriorMap = nh.advertise<sensor_msgs::PointCloud2>("/prior_map", 1);
    pubPriorPath = nh.advertise<sensor_msgs::PointCloud2>("/prior_path", 1);
    pubReloCloud = nh.advertise<sensor_msgs::PointCloud2>("/relo_cloud", 1);
    pubInitCloud = nh.advertise<sensor_msgs::PointCloud2>("/init_cloud", 1);
    pubNearCloud = nh.advertise<sensor_msgs::PointCloud2>("/near_cloud", 1);
    pubMeasurementEdge = nh.advertise<visualization_msgs::MarkerArray>("measurement", 1);
    pubPath = nh.advertise<nav_msgs::Path>("/path_loc", 1e00000);
    std::cout << ANSI_COLOR_GREEN << "rostopic is ok" << ANSI_COLOR_RESET << std::endl;
    
    sessions.push_back(MultiSession::Session(1, "priorMap", priorDir, true));
    *priorMap += *sessions[0].globalMap;
    *priorPath += *sessions[0].cloudKeyPoses3D;
    kdtreeGlobalMapPoses->setInputCloud(priorPath);
    std::cout << ANSI_COLOR_GREEN << "load prior knowledge" << ANSI_COLOR_RESET << std::endl;

    reg.push_back(Registeration(regMode));
}

void pose_estimator::allocateMemory(){
    priorMap.reset(new pcl::PointCloud<PointType>());
    priorPath.reset(new pcl::PointCloud<PointType>());
    reloCloud.reset(new pcl::PointCloud<PointType>());
    initCloud.reset(new pcl::PointCloud<PointType>());
    initCloud_.reset(new pcl::PointCloud<PointType>());
    nearCloud.reset(new pcl::PointCloud<PointType>());
    kdtreeGlobalMapPoses.reset(new pcl::KdTreeFLANN<PointType>());
}

void pose_estimator::cloudCBK(const sensor_msgs::PointCloud2::ConstPtr& msg){
    pcl::PointCloud<PointType>::Ptr msgCloud(new pcl::PointCloud<PointType>());
    pcl::fromROSMsg(*msg, *msgCloud);
    cloudBuffer.push_back(msgCloud);
}

void pose_estimator::poseCBK(const nav_msgs::Odometry::ConstPtr& msg){
    PointTypePose pose;
    pose.x = msg->pose.pose.position.x;
    pose.y = msg->pose.pose.position.y;
    pose.z = msg->pose.pose.position.z;


    Eigen::Vector4d q(msg->pose.pose.orientation.x,
                      msg->pose.pose.orientation.y,
                      msg->pose.pose.orientation.z,
                      msg->pose.pose.orientation.w);
    quaternionNormalize(q);

    Eigen::Matrix3d rot = quaternionToRotation(q);
    Eigen::Matrix<double, 3, 1> euler = RotMtoEuler(rot);
    pose.roll = euler(0,0);
    pose.pitch = euler(1,0);
    pose.yaw = euler(2,0);

    poseBuffer_6D.push_back(pose);
    
    PointType pose3d;
    pose3d.x = msg->pose.pose.position.x;
    pose3d.y = msg->pose.pose.position.y;
    pose3d.z = msg->pose.pose.position.z;

    poseBuffer_3D.push_back(pose3d);
}

void pose_estimator::externalCBK(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg){
    if(external_flg){
        return ;
    }
    std::cout << ANSI_COLOR_RED << "please set your external pose now ... " << ANSI_COLOR_RESET << std::endl;

    externalPose.x = msg->pose.pose.position.x;
    externalPose.y = msg->pose.pose.position.y;
    externalPose.z = 0.0;
    double roll, pitch, yaw;
    tf::Quaternion q;
    tf::quaternionMsgToTF(msg->pose.pose.orientation, q);
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
    externalPose.roll = 0.0;   // FIXME: it's better to choose zero
    externalPose.pitch = 0.0;
    externalPose.yaw = yaw;
    std::cout << ANSI_COLOR_GREEN << "Get initial pose: " << externalPose.x << " " << externalPose.y << " " << externalPose.z 
              << " " << externalPose.roll << " " << externalPose.pitch << " " << externalPose.yaw << ANSI_COLOR_RESET << std::endl;
    external_flg = true;
}

void pose_estimator::run(){
    ros::Rate rate(10);
    while(ros::ok()){
        ros::spinOnce();
        if(!global_flg){
            if(cout_count_ < 1)
                std::cout << ANSI_COLOR_RED << "wait for global pose initialization ... " << ANSI_COLOR_RESET << std::endl;

            global_flg = globalRelo();
            cout_count_ = 1;
            continue;
        }

        if(idx > cloudBuffer.size()){
            std::cout << ANSI_COLOR_RED << "relo > subscribe ... " << ANSI_COLOR_RESET << std::endl;
            continue;
        }

        // if(easyToRelo(poseBuffer_3D[idx])){
        if(0){
            std::cout << ANSI_COLOR_GREEN << "relo mode for frame: " << idx << ANSI_COLOR_RESET << std::endl;
            
            pcl::PointCloud<PointType>::Ptr curCloud(new pcl::PointCloud<PointType>());
            *curCloud += *transformPointCloud(cloudBuffer[idx], &initPose);

            nearCloud->clear();
            for(auto& it : idxVec){
                *nearCloud += *transformPointCloud(sessions[0].cloudKeyFrames[it].all_cloud,
                                                  &sessions[0].cloudKeyPoses6D->points[it]);
            }

            Eigen::MatrixXd transform = reg[0].run(curCloud, nearCloud);
            Eigen::Matrix3d rot = transform.block(0, 0, 3, 3);
            Eigen::MatrixXd linear = transform.block(0, 3, 3, 1);
            Eigen::Matrix<double, 3, 1> euler = RotMtoEuler(rot);
            PointTypePose pose_icp;
            pose_icp.x = linear(0, 0);
            pose_icp.y = linear(1, 0);
            pose_icp.z = linear(2, 0);
            pose_icp.roll = euler(0, 0);
            pose_icp.pitch = euler(1, 0);
            pose_icp.yaw = euler(2, 0);

            reloCloud->clear();
            *reloCloud += *transformPointCloud(curCloud, &pose_icp);
            publishCloud(&pubReloCloud, reloCloud, ros::Time::now(), "camera_init");

            Eigen::Affine3f trans_buffer = pcl::getTransformation(poseBuffer_6D[idx].x, poseBuffer_6D[idx].y, poseBuffer_6D[idx].z, poseBuffer_6D[idx].roll, poseBuffer_6D[idx].pitch, poseBuffer_6D[idx].yaw);
            Eigen::Affine3f trans_init = pcl::getTransformation(initPose.x, initPose.y, initPose.z, initPose.roll, initPose.pitch, initPose.yaw);
            Eigen::Affine3f trans_res = pcl::getTransformation(pose_icp.x, pose_icp.y, pose_icp.z, pose_icp.roll, pose_icp.pitch, pose_icp.yaw);
            Eigen::Affine3f trans_aft = trans_buffer * trans_init * trans_res;

            float aft[6];
            pcl::getTranslationAndEulerAngles(trans_aft, aft[0], aft[1], aft[2],
                                                                 aft[3], aft[4], aft[5]);

            PointTypePose pose_aft;
            pose_aft.x = aft[0];
            pose_aft.y = aft[1];
            pose_aft.z = aft[2];
            pose_aft.roll = aft[3];
            pose_aft.pitch = aft[4];
            pose_aft.yaw = aft[5];
;
            reloPoseBuffer.push_back(pose_aft);

            Eigen::Matrix<double, 3, 3> ang_rot = Exp((double)pose_aft.roll, (double)pose_aft.pitch, (double)pose_aft.yaw);
            Eigen::Vector4d q = rotationToQuaternion(ang_rot);
            quaternionNormalize(q);

            odomAftMapped.pose.pose.position.x = pose_aft.x;
            odomAftMapped.pose.pose.position.y = pose_aft.y;
            odomAftMapped.pose.pose.position.z = pose_aft.z;
            odomAftMapped.pose.pose.orientation.x = q(0);
            odomAftMapped.pose.pose.orientation.y = q(1);
            odomAftMapped.pose.pose.orientation.z = q(2);
            odomAftMapped.pose.pose.orientation.w = q(3);
            publish_odometry(pubPose);

            msg_body_pose.pose.position.x = pose_aft.x;
            msg_body_pose.pose.position.y = pose_aft.y;
            msg_body_pose.pose.position.z = pose_aft.z;
            msg_body_pose.pose.orientation.x = q(0);
            msg_body_pose.pose.orientation.y = q(1);
            msg_body_pose.pose.orientation.z = q(2);
            msg_body_pose.pose.orientation.w = q(3);
            publish_path(pubPath);

            idx ++;
        }
        else{
            std::cout << ANSI_COLOR_GREEN << "lio mode for frame: " << idx << ANSI_COLOR_RESET << std::endl;
            
            pcl::PointCloud<PointType>::Ptr curCloud(new pcl::PointCloud<PointType>());
            *curCloud += *transformPointCloud(cloudBuffer[idx], &initPose);
            *sessions[0].globalMap += *curCloud;
            *priorMap += *curCloud;

            reloCloud->clear();
            *reloCloud += *curCloud;
            publishCloud(&pubReloCloud, reloCloud, ros::Time::now(), "camera_init");

            KeyFrame newFrame;
            *newFrame.all_cloud += *curCloud;
            sessions[0].cloudKeyFrames.push_back(newFrame);

            pcl::PointCloud<PointType>::Ptr invCloud(new pcl::PointCloud<PointType>());
            PointTypePose tmp_zero;
            tmp_zero.x = 0.0;
            tmp_zero.y = 0.0;
            tmp_zero.z = 0.0;
            tmp_zero.roll = 0.0;
            tmp_zero.pitch = 0.0;
            tmp_zero.yaw = 0.0;
            *invCloud += *getBodyCloud(cloudBuffer[idx], poseBuffer_6D[idx], tmp_zero);
            sessions[0].scManager.makeAndSaveScancontextAndKeys(*invCloud);


            Eigen::Affine3f trans_buffer = pcl::getTransformation(poseBuffer_6D[idx].x, poseBuffer_6D[idx].y, poseBuffer_6D[idx].z, poseBuffer_6D[idx].roll, poseBuffer_6D[idx].pitch, poseBuffer_6D[idx].yaw);
            Eigen::Affine3f trans_init = pcl::getTransformation(initPose.x, initPose.y, initPose.z, initPose.roll, initPose.pitch, initPose.yaw);
            Eigen::Affine3f trans_aft = trans_buffer * trans_init;

            float aft[6];
            pcl::getTranslationAndEulerAngles(trans_aft, aft[0], aft[1], aft[2],
                                                                 aft[3], aft[4], aft[5]);

            PointTypePose pose_aft;
            pose_aft.x = aft[0];
            pose_aft.y = aft[1];
            pose_aft.z = aft[2];
            pose_aft.roll = aft[3];
            pose_aft.pitch = aft[4];
            pose_aft.yaw = aft[5];

            reloPoseBuffer.push_back(pose_aft);

            sessions[0].cloudKeyPoses6D->points.push_back(pose_aft);

            PointType pose3d;
            pose3d.x = pose_aft.x;
            pose3d.y = pose_aft.y;
            pose3d.z = pose_aft.z;
            sessions[0].cloudKeyPoses3D->points.push_back(pose3d);
            priorPath->points.push_back(pose3d);

            Eigen::Matrix<double, 3, 3> ang_rot = Exp((double)pose_aft.roll, (double)pose_aft.pitch, (double)pose_aft.yaw);
            Eigen::Vector4d q = rotationToQuaternion(ang_rot);
            quaternionNormalize(q);
            
            odomAftMapped.pose.pose.position.x = pose_aft.x;
            odomAftMapped.pose.pose.position.y = pose_aft.y;
            odomAftMapped.pose.pose.position.z = pose_aft.z;
            odomAftMapped.pose.pose.orientation.x = q(0);
            odomAftMapped.pose.pose.orientation.y = q(1);
            odomAftMapped.pose.pose.orientation.z = q(2);
            odomAftMapped.pose.pose.orientation.w = q(3);
            publish_odometry(pubPose);

            msg_body_pose.pose.position.x = pose_aft.x;
            msg_body_pose.pose.position.y = pose_aft.y;
            msg_body_pose.pose.position.z = pose_aft.z;
            msg_body_pose.pose.orientation.x = q(0);
            msg_body_pose.pose.orientation.y = q(1);
            msg_body_pose.pose.orientation.z = q(2);
            msg_body_pose.pose.orientation.w = q(3);
            publish_path(pubPath);

            idx ++;
        }

        rate.sleep();
    }
}

void pose_estimator::publishThread(){
    bool status = ros::ok();
    ros::Rate rate(1);
    while(status){
        ros::spinOnce();
        publishCloud(&pubPriorMap, priorMap, ros::Time::now(), "camera_init");
        publishCloud(&pubPriorPath, priorPath, ros::Time::now(), "camera_init");
        publishCloud(&pubInitCloud, initCloud, ros::Time::now(), "camera_init");
        publishCloud(&pubNearCloud, nearCloud, ros::Time::now(), "camera_init");
        rate.sleep();
    }
}

bool pose_estimator::easyToRelo(const PointType& pose3d){
    idxVec.clear();
    disVec.clear();
    kdtreeGlobalMapPoses->radiusSearch(pose3d, searchDis, idxVec, disVec);
    if(idxVec.size() >= searchNum){
        // std::cout << ANSI_COLOR_GREEN << "relo mode start for frame " << idx << ANSI_COLOR_RESET << std::endl;
        return true;
    }
    else{
        // std::cout << ANSI_COLOR_GREEN << "lio mode start for frame " << idx << ANSI_COLOR_RESET << std::endl;
        return false;
    }
}

bool pose_estimator::globalRelo(){
    if(cloudBuffer.size() < 1 || poseBuffer_6D.size() < 1){
        if(buffer_flg){
            std::cout << ANSI_COLOR_RED << "wait for cloud and pose from fast-lio2 ... " << ANSI_COLOR_RESET << std::endl;
            buffer_flg = false;
        }
        return false;
    }

    if(!sc_flg){
        *initCloud_ += *cloudBuffer[0];
        std::cout << "init cloud size: " << initCloud_->points.size() << std::endl;
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/cur.pcd", *initCloud_);

        std::cout << ANSI_COLOR_GREEN << "global relo by sc ... " << ANSI_COLOR_RESET << std::endl;
        sessions[0].scManager.makeAndSaveScancontextAndKeys(*initCloud_);
        detectResult = sessions[0].scManager.detectLoopClosureID();
        sessions[0].scManager.polarcontexts_.pop_back();
        sessions[0].scManager.polarcontext_invkeys_.pop_back();
        sessions[0].scManager.polarcontext_vkeys_.pop_back();
        sessions[0].scManager.polarcontext_invkeys_mat_.pop_back();

        // Eigen::MatrixXd initSC = sessions[0].scManager.makeScancontext(*initCloud_);
        // Eigen::MatrixXd ringkey = sessions[0].scManager.makeRingkeyFromScancontext(initSC);
        // Eigen::MatrixXd sectorkey = sessions[0].scManager.makeSectorkeyFromScancontext(initSC);
        // std::vector<float> polarcontext_invkey_vec = ScanContext::eig2stdvec(ringkey);
        // auto detectResult = sessions[0].scManager.detectLoopClosureIDBetweenSession(polarcontext_invkey_vec, initSC);
    
        std::cout << "init relocalization by SC in prior map's id: " << detectResult.first << " yaw offset: " << detectResult.second << std::endl;
        
        // initCloud->clear();
        // *initCloud += *initCloud_;

        if(detectResult.first != -1){
            nearCloud->clear();
            *nearCloud += *sessions[0].cloudKeyFrames[detectResult.first].all_cloud;
        }

        sc_flg = true;

        return false;
    }

    if(!external_flg){
        if(cout_count <= 0){
            std::cout << ANSI_COLOR_RED << "wait for external pose ... " << ANSI_COLOR_RESET << std::endl;
            
        }

        cout_count = 1;
        return false;
    }

    std::cout << ANSI_COLOR_GREEN << "global relocalization processing ... " << ANSI_COLOR_RESET << std::endl;
    
    bool trust;
    PointTypePose poseSC = sessions[0].cloudKeyPoses6D->points[detectResult.first];
    if(detectResult.first < 0){
        trust = false;
        std::cout << ANSI_COLOR_RED << "can not relo by SC ... " << ANSI_COLOR_RESET << std::endl;
    }
    else{
        float x_diff = externalPose.x - poseSC.x;
        float y_diff = externalPose.y - poseSC.y;
        float dis = std::sqrt(x_diff * x_diff + y_diff * y_diff);
        bool trust = (dis <= trustDis) ? false : true;  // select SC-pose or extermal-pose
    }
    
    if(trust){
        std::cout << ANSI_COLOR_GREEN << "init relo by SC-pose ... " << ANSI_COLOR_RESET << std::endl;
        pcl::PointCloud<PointType>::Ptr initCloudCompensate(new pcl::PointCloud<PointType>());
        PointTypePose compensate;
        compensate.x = 0.0;
        compensate.y = 0.0;
        compensate.z = 0.0;
        compensate.roll = 0.0;
        compensate.pitch = 0.0;
        compensate.yaw = detectResult.second;
        *initCloudCompensate += *transformPointCloud(initCloud_, &compensate);
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/com.pcd", *initCloudCompensate);
        std::cout << "init cloud compendate size: " << initCloudCompensate->points.size() << std::endl;
        
        std::cout << ANSI_COLOR_GREEN << "use prior frame " << detectResult.first << " to relo init cloud ..." << ANSI_COLOR_RESET << std::endl;

        nearCloud->clear();
        PointType tmp;
        tmp.x = poseSC.x;
        tmp.y = poseSC.y;
        tmp.z = poseSC.z;
        idxVec.clear();
        disVec.clear();
        kdtreeGlobalMapPoses->nearestKSearch(tmp, searchNum, idxVec, disVec);
        *nearCloud += *sessions[0].cloudKeyFrames[idxVec[0]].all_cloud;
        for(int i = 1; i < idxVec.size(); i++){
            *nearCloud += *getBodyCloud(sessions[0].cloudKeyFrames[idxVec[i]].all_cloud, poseSC, sessions[0].cloudKeyPoses6D->points[idxVec[i]]);
        }
        std::cout << "near cloud size: " << nearCloud->points.size() << std::endl;
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/near.pcd", *nearCloud);
        
        std::cout << ANSI_COLOR_GREEN << "get precise pose by FR-ICP ... " << ANSI_COLOR_RESET << std::endl;
        Eigen::MatrixXd transform = reg[0].run(initCloudCompensate, nearCloud);
        Eigen::Matrix3d rot = transform.block(0, 0, 3, 3);
        Eigen::MatrixXd linear = transform.block(0, 3, 3, 1);
        Eigen::Matrix<double, 3, 1> euler = RotMtoEuler(rot);

        PointTypePose poseReg;
        poseReg.x = linear(0, 0);
        poseReg.y = linear(1, 0);
        poseReg.z = linear(2, 0);
        poseReg.roll = euler(0, 0);
        poseReg.pitch = euler(1, 0);
        poseReg.yaw = euler(2, 0);
        
        PointTypePose poseOffset;
        poseOffset.x = poseSC.x;
        poseOffset.y = poseSC.y;
        poseOffset.z = poseSC.z;
        poseOffset.roll = 0.0;
        poseOffset.pitch = 0.0;
        poseOffset.yaw = 0.0;

        initCloud->clear();
        *initCloud += *getAddCloud(initCloudCompensate, poseReg, poseOffset);
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/init.pcd", *initCloud);

        Eigen::Affine3f trans_com = pcl::getTransformation(compensate.x, compensate.y, compensate.z, compensate.roll, compensate.pitch, compensate.yaw);
        Eigen::Affine3f trans_reg = pcl::getTransformation(poseReg.x, poseReg.y, poseReg.z, poseReg.roll, poseReg.pitch, poseReg.yaw);
        Eigen::Affine3f trans_offset = pcl::getTransformation(poseOffset.x, poseOffset.y, poseOffset.z, poseOffset.roll, poseOffset.pitch, poseOffset.yaw);
        Eigen::Affine3f trans_init = trans_com * trans_reg * trans_offset;

        float pose_init[6];
        pcl::getTranslationAndEulerAngles(trans_init, pose_init[0], pose_init[1], pose_init[2],
                                                                 pose_init[3], pose_init[4], pose_init[5]);
        initPose.x = pose_init[0];
        initPose.y = pose_init[1];
        initPose.z = pose_init[2];
        initPose.roll = pose_init[3];
        initPose.pitch = pose_init[4];
        initPose.yaw = pose_init[5];
        
        global_flg = true;
        std::cout << ANSI_COLOR_GREEN << "Get optimized pose: " << initPose.x << " " << initPose.y << " " << initPose.z 
              << " " << initPose.roll << " " << initPose.pitch << " " << initPose.yaw << ANSI_COLOR_RESET << std::endl;
        std::cout << ANSI_COLOR_RED << "init relocalization has been finished ... " << ANSI_COLOR_RESET << std::endl;

        return true;
    }
    else{
        std::cout << ANSI_COLOR_GREEN << "init relo by external-pose ... " << ANSI_COLOR_RESET << std::endl;
        PointType tmp;
        tmp.x = externalPose.x;
        tmp.y = externalPose.y;
        tmp.z = externalPose.z;
        if(!easyToRelo(tmp)){
            external_flg = false;
            std::cout << ANSI_COLOR_RED << "please reset external pose ... " << ANSI_COLOR_RESET << std::endl;
            return false;
        }

        std::cout << ANSI_COLOR_GREEN << "use prior frame " << idxVec[0] << " to relo init cloud ..." << ANSI_COLOR_RESET << std::endl;
        PointTypePose pose_new = sessions[0].cloudKeyPoses6D->points[idxVec[0]];

        pcl::PointCloud<PointType>::Ptr initCloudInv(new pcl::PointCloud<PointType>());
        PointTypePose tmp_zero;
        tmp_zero.x = 0.0;
        tmp_zero.y = 0.0;
        tmp_zero.z = 0.0;
        tmp_zero.roll = 0.0;
        tmp_zero.pitch = 0.0;
        tmp_zero.yaw = 0.0;

        *initCloudInv += *getBodyCloud(initCloud_, pose_new, tmp_zero);
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/inv.pcd", *initCloudInv);

        pcl::PointCloud<PointType>::Ptr initCloudCompensate(new pcl::PointCloud<PointType>());
        PointTypePose compensate;
        compensate.x = 0.0;
        compensate.y = 0.0;
        compensate.z = 0.0;
        compensate.roll = 0.0;
        compensate.pitch = 0.0;
        compensate.yaw = externalPose.yaw;

        *initCloudCompensate += *transformPointCloud(initCloudInv, &compensate);
        std::cout << "init cloud compendate size: " << initCloudCompensate->points.size() << std::endl;
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/com.pcd", *initCloudCompensate);

        PointType tmp2;
        tmp2.x = pose_new.x;
        tmp2.y = pose_new.y;
        tmp2.z = pose_new.z;
        idxVec.clear();
        disVec.clear();
        kdtreeGlobalMapPoses->nearestKSearch(tmp2, searchNum, idxVec, disVec);
        nearCloud->clear();
        *nearCloud += *(sessions[0].cloudKeyFrames[idxVec[0]].all_cloud);
        for(int i = 1; i < idxVec.size(); i++){
            *nearCloud += *getBodyCloud(sessions[0].cloudKeyFrames[idxVec[i]].all_cloud, pose_new, sessions[0].cloudKeyPoses6D->points[idxVec[i]]);
        }
        std::cout << "near cloud size: " << nearCloud->points.size() << std::endl;
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/near.pcd", *nearCloud);
        
        std::cout << ANSI_COLOR_GREEN << "get precise pose by FR-ICP ... " << ANSI_COLOR_RESET << std::endl;
        Eigen::MatrixXd transform = reg[0].run(initCloudCompensate, nearCloud);
        Eigen::Matrix3d rot = transform.block(0, 0, 3, 3);
        Eigen::MatrixXd linear = transform.block(0, 3, 3, 1);
        // std::cout << "linear: " << linear << std::endl;

        Eigen::Matrix<double, 3, 1> euler = RotMtoEuler(rot);

        PointTypePose poseReg;
        poseReg.x = linear(0, 0);
        poseReg.y = linear(1, 0);
        poseReg.z = linear(2, 0);
        poseReg.roll = euler(0, 0);
        poseReg.pitch = euler(1, 0);
        poseReg.yaw = euler(2, 0);

        // std::cout << poseReg.x << " " << poseReg.y << " " << poseReg.z << std::endl;
        // std::cout << poseReg.roll << " " << poseReg.pitch << " " << poseReg.yaw << std::endl;
        
        PointTypePose poseOffset;
        poseOffset.x = pose_new.x;
        poseOffset.y = pose_new.y;
        poseOffset.z = pose_new.z;
        poseOffset.roll = 0.0;
        poseOffset.pitch = 0.0;
        poseOffset.yaw = 0.0;

        initCloud->clear();
        *initCloud += *getAddCloud(initCloudCompensate, poseReg, poseOffset);
        pcl::io::savePCDFile("/home/yixin-f/fast-lio2/src/data_loc/init.pcd", *initCloud);

        Eigen::Affine3f trans_com = pcl::getTransformation(compensate.x, compensate.y, compensate.z, compensate.roll, compensate.pitch, compensate.yaw);
        Eigen::Affine3f trans_reg = pcl::getTransformation(poseReg.x, poseReg.y, poseReg.z, poseReg.roll, poseReg.pitch, poseReg.yaw);
        Eigen::Affine3f trans_offset = pcl::getTransformation(poseOffset.x, poseOffset.y, poseOffset.z, poseOffset.roll, poseOffset.pitch, poseOffset.yaw);
        Eigen::Affine3f trans_init = trans_com * trans_reg * trans_offset;

        float pose_init[6];
        pcl::getTranslationAndEulerAngles(trans_init, pose_init[0], pose_init[1], pose_init[2],
                                                                 pose_init[3], pose_init[4], pose_init[5]);
        initPose.x = pose_init[0];
        initPose.y = pose_init[1];
        initPose.z = pose_init[2];
        initPose.roll = pose_init[3];
        initPose.pitch = pose_init[4];
        initPose.yaw = pose_init[5];

        global_flg = true;
        std::cout << ANSI_COLOR_GREEN << "Get optimized pose: " << initPose.x << " " << initPose.y << " " << initPose.z 
              << " " << initPose.roll << " " << initPose.pitch << " " << initPose.yaw << ANSI_COLOR_RESET << std::endl;
        std::cout << ANSI_COLOR_RED << "init relocalization has been finished ... " << ANSI_COLOR_RESET << std::endl;

        return true;
    }
    
}

void pose_estimator::publish_odometry(const ros::Publisher &pubOdomAftMapped){
    odomAftMapped.header.frame_id = "camera_init";
    odomAftMapped.child_frame_id = "loc";
    odomAftMapped.header.stamp = ros::Time::now();
    pubOdomAftMapped.publish(odomAftMapped);

    // static tf::TransformBroadcaster br;
    // tf::Transform transform;
    // tf::Quaternion q;
    // transform.setOrigin(tf::Vector3(odomAftMapped.pose.pose.position.x,
    //                                 odomAftMapped.pose.pose.position.y,
    //                                 odomAftMapped.pose.pose.position.z));
    // q.setW(odomAftMapped.pose.pose.orientation.w);
    // q.setX(odomAftMapped.pose.pose.orientation.x);
    // q.setY(odomAftMapped.pose.pose.orientation.y);
    // q.setZ(odomAftMapped.pose.pose.orientation.z);
    // transform.setRotation(q);
    // br.sendTransform(tf::StampedTransform(transform, odomAftMapped.header.stamp, "camera_init", "loc"));
}

void pose_estimator::publish_path(const ros::Publisher& pubPath){
    msg_body_pose.header.stamp = ros::Time::now();
    msg_body_pose.header.frame_id = "camera_init";
    path.poses.push_back(msg_body_pose);
    pubPath.publish(path);
}