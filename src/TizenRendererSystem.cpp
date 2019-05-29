#include <dali-toolkit/dali-toolkit.h>
#include <dali/dali.h>
#include <bullet/btBulletDynamicsCommon.h>
#include <iostream>
#include <list>
#include <string>
#include <chrono>
#include "Assets.h"
#include "FrameActor.h"
#include "PhysicsActor.h"
#include "Shader.h"
#include "PrimitiveModels.h"
#include "TUM.h"
#include "ORB-SLAM2/System.h"
#include "ORB-SLAM2/Converter.h"
#include "Background.h"
#include "Scene.h"
#include "DebugScene.h"
#include "RealSense.h"
#include "FileSystem.h"
#include "Net.h"

#include <dlog.h>
#include <sstream>

const unsigned int UPDATE_INTERVAL = 64;
const float Focal_X = 517.306408f;  // TUM
const float Focal_Y = 516.469215f;  // TUM
const int SCREEN_WIDTH = 960;
const int SCREEN_HEIGHT = 720;
const float CAMERA_NEAR = 0.001f;
const float CAMERA_FAR = 2000.0f;
const float CAMERA_ASPECT = (SCREEN_WIDTH * Focal_Y) / (SCREEN_HEIGHT * Focal_X);
const float CAMERA_FOV = atanf(SCREEN_HEIGHT / (2.0f * Focal_Y)) * 2;

class TizenRendererSystem : public Dali::ConnectionTracker
{
public:
    TizenRendererSystem(Dali::Application &application)
        : mApplication(application)
    {
        Assets::Init();
        dlog_print(DLOG_DEBUG, "TIZENAR", "asset init");
        InitBullet();
        dlog_print(DLOG_DEBUG, "TIZENAR", "bullet init");
        mApplication.InitSignal().Connect(this, &TizenRendererSystem::Create);
    }

    void Dispose()
    {
        mScene->Dispose();
        delete mDynamicsWorld;
    }

private:
    void Create(Dali::Application &application)
    {
    	dlog_print(DLOG_DEBUG, "TIZENAR", "create start");
        Dali::Window winHandle = application.GetWindow();
        winHandle.ShowIndicator( Dali::Window::INVISIBLE );

        // create scene
        mStage = Dali::Stage::GetCurrent();
        mStage.KeyEventSignal().Connect( this, &TizenRendererSystem::OnKeyEvent );

        //string backgroundPath = "../res/images/001.jpg";
        string backgroundPath = FileSystem::GetResourcePath("images/001.jpg");
        CreateBackgroundImg(backgroundPath);
        dlog_print(DLOG_DEBUG, "TIZENAR", "background created");

        mStage.GetRootLayer().SetBehavior(Dali::Layer::LAYER_3D);
        
        // Camera default transform
        // Initial rotation is (0, 180, 0)
        mCamera = mStage.GetRenderTaskList().GetTask(0).GetCameraActor();
        mStage.GetRenderTaskList().GetTask(0).SetCullMode( false );
        mCamera.SetNearClippingPlane(CAMERA_NEAR);
        mCamera.SetFarClippingPlane(CAMERA_FAR);
        cout << "camera fov, asepct : " << mCamera.GetFieldOfView() << ", " << mCamera.GetAspectRatio() << endl;
        mCamera.SetAspectRatio(CAMERA_ASPECT);
        mCamera.SetFieldOfView(CAMERA_FOV);
        mCamera.SetAnchorPoint(AnchorPoint::CENTER);
        mCamera.SetParentOrigin(ParentOrigin::CENTER);
        mCamera.SetPosition(Dali::Vector3(0, 0, 0));
        mCamera.SetOrientation(Quaternion(1, 0, 0, 0));
        InitUILayer();
        mUILayer.TouchSignal().Connect(this, &TizenRendererSystem::OnTouch);
        dlog_print(DLOG_DEBUG, "TIZENAR", "camera, ui create");

        // Set update loop
		mTimer = Dali::Timer::New(UPDATE_INTERVAL);
		mTimer.TickSignal().Connect(this, &TizenRendererSystem::Update);
		mTimer.Start();
		dlog_print(DLOG_DEBUG, "TIZENAR", "timer start");

        CreatePlane();

        mScene = new DebugScene(mStage, mCamera, mUILayer, mDynamicsWorld, mPlane);
        dlog_print(DLOG_DEBUG, "TIZENAR", "create done");
    }

    bool Update()
    {
        static int _updateCount = 0;

        double deltaTime = 0.0;
        if (_updateCount < 1)
        {
            // First frame that Dali Actors are not initialized yet
            ++_updateCount;
        }
        else if (_updateCount < 2)
        {
            // Second frame right after Dali Actors are initialized
            ++_updateCount;
            mInitTime = std::chrono::high_resolution_clock::now();
            mOldTime = mInitTime;
            mCurrentTime = mInitTime;
            Net::BeginClient(9999); // blocked until connection
        }
        else
        {
            // Get elapsed time measured in seconds
            mCurrentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = mCurrentTime - mOldTime;
            deltaTime = elapsed.count();

            if(ReceiveData())
            {
            	UpdateBackgroundMat(_rgb);
            	UpdateCamera();
            }

            
            if(mUpdatePlane)
            {
            	if(ReceivePlane())
            	{
            		cout << "calc plane" << endl;
					dlog_print(DLOG_DEBUG, "TIZENAR", "plane update");
					wVector3 normal( Eigen::Vector3f(_planeEq(0), _planeEq(1), _planeEq(2)) );
					normal.Normalize();
					if (normal.y < 0) normal = wVector3(-normal.x, -normal.y, -normal.z);

					Dali::Vector3 n = normal.ToDali();
					Dali::Vector3 z = Dali::Vector3(-1, 0, 0).Cross(n);
					Dali::Vector3 x = n.Cross(z);
					wQuaternion rotation( Dali::Quaternion(x, n, z) );
					//wQuaternion rotation( Dali::Quaternion(Dali::Vector3(0, 1, 0), normal.ToDali()) );
					mPlane->SetPosition(wVector3(_planePos));
					mPlanePos = wVector3(_planePos).ToDali();
					mPlane->SetRotation(rotation);
					mPlaneRot = rotation.ToDali();

//					std::stringstream ss, ss2;
//					ss << "Plane pos : " << mPlanePos;
//					ss2 << "Plane rot : " << mPlaneRot.mVector;
//					dlog_print(DLOG_DEBUG, "TIZENAR", ss.str().c_str());
//					dlog_print(DLOG_DEBUG, "TIZENAR", ss2.str().c_str());

					float gravity = normal.y > 0 ? -9.81f : 9.81f;
					mDynamicsWorld->setGravity(normal.ToBullet() * gravity);

					OnPlaneUpdated();
            	}
            }

            if(mSceneStart) mScene->Update(deltaTime);
        }

        mOldTime = mCurrentTime;
        return true;
    }

    bool ReceiveData()
	{
		if (not Net::IsConnected()) return false;
		Net::Send(Net::ID_CAM, nullptr, 0);
		if (not Net::Receive()) return false;

		std::cout << "Receive " << Net::GetTotalLength() << " bytes" << std::endl;
		char *buf = Net::GetData();

		Net::Mat output_left;
		Net::Vec3 output_pos;
		Net::Vec4 output_rot;
		Net::DecodeCameraData(buf, output_left, output_pos, output_rot);

		// 1. create cv::Mat from received data first
		_rgb = cv::Mat(SCREEN_HEIGHT, SCREEN_WIDTH, CV_8UC3, output_left.data);
		// 2. output_left.data will be freed out of this scope
		// 3. cv::Mat::clone() lets cv::Mat own its buffer so that _rgb will maintain its value
		_rgb = _rgb.clone();

		_camPos = wVector3( Dali::Vector3(output_pos.x, output_pos.y, output_pos.z) );
		_camRot = wQuaternion( Dali::Quaternion(output_rot.w, output_rot.x, output_rot.y, output_rot.z) );

		delete[] buf;
		return true;
	}

    bool ReceivePlane()
    {
    	if (not Net::IsConnected()) return false;
    	Net::Send(Net::ID_PLANE, nullptr, 0);
    	if (not Net::Receive()) return false;

    	char *buf = Net::GetData();

    	Net::Vec4 eq;
    	Net::Vec3 pos;
    	Net::DecodePlaneData(buf, eq, pos);

    	_planeEq = Eigen::Vector4f(eq.x, eq.y, eq.z, eq.w);
    	_planePos = Eigen::Vector3f(pos.x, pos.y, pos.z);

    	delete[] buf;
    	return true;
    }

    void OnKeyEvent( const KeyEvent& event )
    {
        if( event.state == KeyEvent::Down )
        {
            // cout << event.keyCode << endl;
            if ( IsKey( event, Dali::DALI_KEY_ESCAPE ) || IsKey( event, Dali::DALI_KEY_BACK ) )
            {
                mApplication.Quit();
            }
        }
        mScene->OnKeyEvent(event);
    }

    bool OnTouch(Dali::Actor actor, const Dali::TouchData &touch)
    {
        mScene->OnTouch(actor, touch);
        return true;
    }

    void OnPlaneUpdated()
    {
        static bool _first = true;
        if (_first)
        {
            // Scene should be started after the main plane is detected.
            mScene->Init();
            dlog_print(DLOG_DEBUG, "TIZENAR", "scene init");
            mScene->OnStart();
            dlog_print(DLOG_DEBUG, "TIZENAR", "scene start");
            mSceneStart = true;
            _first = false;
        }
    }

    void UpdateCamera()
    {
		mCamera.SetPosition( _camPos.ToDali() );
		mCamera.SetOrientation( _camRot.ToDali() );
//		std::stringstream ss, ss2;
//		ss << "camera pos: " << CameraPos.ToDali();
//		ss2 << "camera rot:  " << CameraRot.ToDali().mVector;
//		dlog_print(DLOG_DEBUG, "TIZENAR", ss.str().c_str());
//		dlog_print(DLOG_DEBUG, "TIZENAR", ss2.str().c_str());
    }

    void InitBullet()
    {
        btDefaultCollisionConfiguration *cfg = new btDefaultCollisionConfiguration();
        btCollisionDispatcher *dispatcher = new btCollisionDispatcher(cfg);
        btBroadphaseInterface *overlappingPairCache = new btDbvtBroadphase();
        btSequentialImpulseConstraintSolver *solver = new btSequentialImpulseConstraintSolver();
        mDynamicsWorld = new btDiscreteDynamicsWorld(dispatcher, overlappingPairCache, solver, cfg);
        mDynamicsWorld->setGravity(btVector3(0, -9.81, 0));
    }

    void InitUILayer()
    {
        cout << "init ui" << endl; 
        cout << "current camera pos is : " << mCamera.GetCurrentWorldPosition() << endl;

        float cameraZ = SCREEN_HEIGHT / (2.0f * std::tan(0.5f * CAMERA_FOV));

        mUILayer = Dali::Layer::New();
        cout << "mUILayer init pos is : " << mUILayer.GetCurrentWorldPosition() << endl; 
        cout << "mUILayer init rot is : " << mUILayer.GetCurrentWorldOrientation() << endl;
        mUILayer.SetParentOrigin( Dali::ParentOrigin::CENTER );
        mUILayer.SetAnchorPoint( Dali::AnchorPoint::CENTER );
        mUILayer.SetSize( Dali::Vector2( mStage.GetSize().y * CAMERA_ASPECT, mStage.GetSize().y ) );
        mCamera.Add( mUILayer );
        mUILayer.RaiseToTop();
        mUILayer.SetOrientation(Dali::Quaternion( Dali::Radian(Dali::Degree(180)) , Dali::Vector3(0, 1, 0) ));
        mUILayer.SetPosition(Dali::Vector3(0, 0, cameraZ));
    }

    void CreatePlane()
    {
        mPlaneShader = LoadShaders("vertexColor.glsl", "fragmentColor.glsl");
        mPlaneShader.RegisterProperty("uAlpha", 0.3f);


        PrimitiveCube floorModel("wood.png", mPlaneShader);
        
        mPlane = new PhysicsActor(mStage, floorModel, mDynamicsWorld);
        mPlane->SetName("Plane");
        mPlane->SetPosition(0, 0, 0);
        mPlane->SetSize( 1, 0.05, 1 );

        // plane position for debugging
		// mPlane->SetPosition(wVector3(Dali::Vector3(0.0504673, 0.0932271, 1.283)));
		// mPlane->SetRotation(wQuaternion(Dali::Quaternion(-0.244961, 0.740481, 0.193315, -0.595241)));
    }



private:
    // Application
    Dali::Application &mApplication;

    Scene *mScene;
    btDiscreteDynamicsWorld *mDynamicsWorld;
    Dali::Stage mStage;
    Dali::CameraActor mCamera;
    Dali::Timer mTimer;
    bool mSceneStart = false;

    // UI
    Dali::Toolkit::Control mUIControl;
    Dali::Layer mUILayer;

    // Plane
    Dali::Shader mPlaneShader;
    PhysicsActor* mPlane;
    Dali::Vector3 mPlanePos;
    Dali::Quaternion mPlaneRot;
    bool mUpdatePlane = true;

    // Time 
    std::chrono::time_point<std::chrono::high_resolution_clock> mCurrentTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> mOldTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> mInitTime;

    // Data from server
    wVector3 _camPos;
    wQuaternion _camRot;
    Eigen::Vector4f _planeEq;
    Eigen::Vector3f _planePos;
    cv::Mat _rgb;
};

// main function
int DALI_EXPORT_API main(int argc, char **argv)
{
    Dali::Application application = Dali::Application::New(&argc, &argv);
    TizenRendererSystem sys(application);
    application.MainLoop();
    sys.Dispose();
}
