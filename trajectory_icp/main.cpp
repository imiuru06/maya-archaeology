#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/point_cloud.h>
#include <pcl/console/parse.h>
#include <pcl/common/transforms.h>
#include <pcl/octree/octree.h>
#include <pcl/registration/icp.h>
#include <pcl/common/common.h>
#include <pcl/filters/crop_box.h>

#include <pcl/visualization/pcl_visualizer.h>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <thread>
#include <mutex>

#include <Eigen/StdVector>

EIGEN_DEFINE_STL_VECTOR_SPECIALIZATION(Eigen::Affine3f)


using namespace std;
using namespace pcl;


const int SIZE_X = 640;
const int SIZE_Y = 480;


boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer;
pcl::PointCloud<pcl::PointXYZ>::Ptr slice_cloud;
pcl::PointCloud<pcl::PointXYZ>::Ptr slam_cloud;
pcl::PointCloud<pcl::PointXYZ>::Ptr aligned_slam_cloud;
pcl::PointCloud<pcl::PointXYZ>::Ptr aligned_traj_cloud;
mutex pointcloud_mutex;

PointXYZ depth_map[SIZE_X][SIZE_Y];
int depth_map_idx[SIZE_X][SIZE_Y];


//----------------------------------------------------------
void clearDepthMap()
//----------------------------------------------------------
{
	for (int i = 0; i < SIZE_X; i++)
	{
		for (int j = 0; j < SIZE_Y; j++)
		{
			depth_map[i][j] = PointXYZ();
			depth_map[i][j].z = 999999999;
			depth_map_idx[i][j] = -1;
		}
	}
}


//----------------------------------------------------------
void showViewer(
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud,
		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud2 = 0)
//----------------------------------------------------------
{
	viewer = boost::shared_ptr<pcl::visualization::PCLVisualizer>(new pcl::visualization::PCLVisualizer ("3D Viewer"));
	viewer->setBackgroundColor (0, 0, 0);
	//viewer->addPointCloud<PointXYZ> (slice_cloud, "Cloud 1");
	//viewer->addPointCloud<PointXYZRGB> (trajectory_cloud, "Cloud 2");
	viewer->addPointCloud<PointXYZ> (cloud, "Cloud 1");
	if (cloud2 != 0) { viewer->addPointCloud<PointXYZRGB> (cloud2, "Cloud 2"); }
	pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ> single_color(slam_cloud, 0, 255, 0);
	pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ> single_color2(slam_cloud, 0, 0, 255);
	pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ> single_color3(slam_cloud, 0, 255, 255);
	viewer->addPointCloud<PointXYZ> (slam_cloud, single_color, "slam_cloud");
	viewer->addPointCloud<PointXYZ> (aligned_slam_cloud, single_color2, "aligned_cloud");
	viewer->addPointCloud<PointXYZ> (aligned_traj_cloud, single_color3, "aligned_traj_cloud");
	viewer->setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "Cloud 1");
	viewer->addCoordinateSystem (1.0);
	viewer->initCameraParameters ();

	while (!viewer->wasStopped ())
	{
		boost::this_thread::sleep (boost::posix_time::microseconds (100000));
		lock_guard<mutex> guard(pointcloud_mutex);
		//viewer->updatePointCloud(slice_cloud, "Cloud 1");
		viewer->removePointCloud("Cloud 1");
		viewer->addPointCloud<PointXYZ> (slice_cloud, "Cloud 1");
		viewer->removePointCloud("slam_cloud");
		viewer->addPointCloud<PointXYZ> (slam_cloud, single_color, "slam_cloud");
		viewer->removePointCloud("aligned_cloud");
		viewer->addPointCloud<PointXYZ> (aligned_slam_cloud, single_color2, "aligned_cloud");
		viewer->removePointCloud("aligned_traj_cloud");
		viewer->addPointCloud<PointXYZ> (aligned_traj_cloud, single_color3, "aligned_traj_cloud");
		viewer->spinOnce (100);
	}
}


//----------------------------------------------------------
void printUsage(const char* progName)
//----------------------------------------------------------
{
	cout << "\n\nUsage: " << progName << " <Traj_File.txt> <tranform.txt> <pointclouds.txt> <pointclouds.pcd> [options] \n\n"
		<< "Options:\n"
		<< "-------------------------------------------\n"
		<< "-h  --help                         This help\n"
		<< "\n\n";
}


//----------------------------------------------------------
int main (int argc, char** argv)
//----------------------------------------------------------
{
	if(pcl::console::find_argument(argc, argv, "-h") >= 0
			|| pcl::console::find_argument(argc, argv, "--help") >= 0)
	{
		printUsage(argv[0]);
		return EXIT_SUCCESS;
	}

	vector<int> txt_filenames = pcl::console::parse_file_extension_argument(argc, argv, "txt");
	vector<int> pcd_filenames = pcl::console::parse_file_extension_argument(argc, argv, "pcd");

	if(txt_filenames.size() < 3 || pcd_filenames.size() < 1)
	{
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	bool display = pcl::console::find_argument(argc, argv, "-d") >= 0;

	string traj_filename = string(argv[txt_filenames[0]]);
	string transform_filename = string(argv[txt_filenames[1]]);
	string pc_filename   = string(argv[txt_filenames[2]]);
	string pcd_filename   = string(argv[pcd_filenames[0]]);


	// ---
	// Read input files
	// ---

	ifstream traj_file(traj_filename);
	ifstream trans_file(transform_filename);
	ifstream pc_file(pc_filename);

	Eigen::Affine3f global_transform;
	float value;
	for (int i = 0; i < 16; i++)
	{
		trans_file >> value;
		cout << value << " ";
		global_transform(i/4,i%4) = value;
	}
	cout << endl;

	cout << global_transform.matrix() << endl;



	string traj_str;
	string pc_str;

	vector<string> paths;
	vector<Eigen::Affine3f> transforms;
	vector<Eigen::Affine3f> aligned_transforms;

	while(pc_file >> pc_str)
	{
		paths.push_back(pc_str);
	}

	while(traj_file >> traj_str)
	{
		float tx, ty, tz, qw, qx, qy, qz;
		traj_file >> tx >> ty >> tz >> qx >> qy >> qz >> qw;

		Eigen::Affine3f transform = Eigen::Affine3f::Identity();
		transform.translation() << tx, ty, tz;

		Eigen::Matrix3f mat3 = Eigen::Quaternionf(qw, qx, qy, qz).toRotationMatrix();
		transform.rotate (mat3);

		transform = global_transform * transform;

		transforms.push_back(transform);
	}



	pcl::PointCloud<pcl::PointXYZ>::Ptr source_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
	pcl::io::loadPCDFile<pcl::PointXYZ> (pcd_filename, *source_cloud);



	pcl::PointCloud<pcl::PointXYZRGB>::Ptr trajectory_cloud (new pcl::PointCloud<pcl::PointXYZRGB> ());
	for (auto& t : transforms)
	{
		PointXYZRGB p;
		p.x = t.translation()[0];
		p.y = t.translation()[1];
		p.z = t.translation()[2];
		p.r = 255;
		p.g = 0;
		p.b = 0;
		trajectory_cloud->push_back(p);
	}


	pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
	pcl::PointCloud<pcl::PointXYZ>::Ptr orig_slam_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
	pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_slam_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
	pcl::PointCloud<pcl::PointXYZ>::Ptr lidar_slice_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
	pcl::PointCloud<pcl::PointXYZ> final_cloud;

	slice_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ> ());
	slam_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ> ());
	aligned_slam_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ> ());
	aligned_traj_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ> ());

	float fx, fy, cx, cy;
	fx = 525.0;
	fy = 525.0;
	cx = 319.5;
	cy = 239.5;
	// These have a factor of 5 IIRC
	float max_dist = 3.0f;
	float margin_dist = 0.1f;


	thread show_thread;
	
	if (display)
	{
		show_thread = thread(showViewer, slice_cloud, trajectory_cloud);
	}


	this_thread::sleep_for(chrono::seconds(1));

	for(int j = 0; j < transforms.size(); j++)
	{
		cout << j+1 << " / " << transforms.size() << endl;

		//this_thread::sleep_for(chrono::milliseconds(200));
	
		auto adjusted_transform = transforms[j];

		if (j > 0)
		{
			adjusted_transform = aligned_transforms[j-1] * (transforms[j-1].inverse() * transforms[j]);
		}

		pcl::io::loadPLYFile (paths[j], *orig_slam_cloud);
		pcl::transformPointCloud (*orig_slam_cloud, *transformed_slam_cloud, adjusted_transform);
		
		PointXYZ slam_min, slam_max;
		getMinMax3D(*transformed_slam_cloud, slam_min, slam_max);
	
		pcl::CropBox<PointXYZ> cropFilter;
		cropFilter.setInputCloud (source_cloud);
		cropFilter.setMin(Eigen::Vector4f(slam_min.x-margin_dist, slam_min.y-margin_dist, slam_min.z-margin_dist, 1.0f) );
		cropFilter.setMax(Eigen::Vector4f(slam_max.x+margin_dist, slam_max.y+margin_dist, slam_max.z+margin_dist, 1.0f) );
   		
		cropFilter.filter (*lidar_slice_cloud);


		//{
		//	lock_guard<mutex> guard(pointcloud_mutex);
   		//	cropFilter.filter (*slice_cloud);
		//	copyPointCloud(*transformed_slam_cloud, *slam_cloud);
		//}



		//pcl::transformPointCloud(*source_cloud, *transformed_cloud, transforms[j].inverse());

		//clearDepthMap();

		//for (int i = 0; i < transformed_cloud->size(); i++)
		//{
		//	auto& p = (*transformed_cloud)[i];
		//	if (p.z < 0 || p.z > max_dist) { continue; }
		//	int px, py;
		//	px = (p.x * fx) / p.z + cx;
		//	py = ((p.y * fy) / p.z + cy);
		//	if (px >= 0 && px < SIZE_X && py >= 0 && py < SIZE_Y)
		//	{
		//		if (depth_map[px][py].z > p.z)
		//		{
		//			depth_map[px][py] = p;
		//			depth_map_idx[px][py] = i;
		//		}
		//	}
		//}
		//
		//{
		//	lock_guard<mutex> guard(pointcloud_mutex);

		//	slice_cloud->clear();
		//	
		//	for (int x = 0; x < SIZE_X; x++)
		//	{
		//		for (int y = 0; y < SIZE_Y; y++)
		//		{
		//			int idx = depth_map_idx[x][y];
		//			if (idx >= 0)
		//			{
		//				slice_cloud->push_back((*source_cloud)[idx]);
		//			}
		//		}
		//	}
		//	
		//	pcl::transformPointCloud (*orig_slam_cloud, *slam_cloud, transforms[j]);
		//}

		
		pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
		icp.setInputSource(transformed_slam_cloud);
		icp.setInputTarget(lidar_slice_cloud);
		
		icp.align(final_cloud);
		
		cout << "Converged: " << icp.hasConverged() << " Score: " << icp.getFitnessScore() << endl;

		auto new_transform = Eigen::Affine3f(icp.getFinalTransformation()) * adjusted_transform;
		PointXYZ p;
		p.x = new_transform.translation()[0];
		p.y = new_transform.translation()[1];
		p.z = new_transform.translation()[2];

		aligned_transforms.push_back(new_transform);

		if (display)
		{
			lock_guard<mutex> guard(pointcloud_mutex);
			copyPointCloud(final_cloud, *aligned_slam_cloud);
			copyPointCloud(*transformed_slam_cloud, *slam_cloud);
			copyPointCloud(*lidar_slice_cloud, *slice_cloud);
			aligned_traj_cloud->push_back(p);
		}
		else
		{
			aligned_traj_cloud->push_back(p);
		}
	}





	if (display)
	{
		show_thread.join();
	}


	// ---
	// Save output
	// ---

	cout << "Saving output trajectories..." << endl;


	pcl::io::savePLYFile("slam_traj.ply", *trajectory_cloud, true);
	pcl::io::savePLYFile("gt_traj.ply", *aligned_traj_cloud, true);

	ofstream traj_out("gt_traj.txt");

	for (auto& t :aligned_transforms)
	{
		traj_out << t.matrix() << "\n";
	}

	return 0;
}