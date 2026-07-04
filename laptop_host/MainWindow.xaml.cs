using System;
using System.ComponentModel;
using System.Windows;
using System.Windows.Threading;

namespace X3LaptopCompanion
{
    public partial class MainWindow : Window, INotifyPropertyChanged
    {
        private readonly TeamsController teamsController = new TeamsController();
        private readonly CompanionConnectionService connectionService = new CompanionConnectionService();
        private readonly DispatcherTimer statusTimer = new DispatcherTimer();

        private string connectionText = "Disconnected";
        private string teamsText = "Waiting for Teams";
        private string microphoneText = "Unknown";
        private string cameraText = "Unknown";
        private bool isTestMode;
        private string testModeText = "Off";
        private bool testTeamsDetected = true;
        private string testMessage = "Test status";
        private string testTeamsToggleText = "Detected";
        private string testMicrophoneToggleText = "Unknown";
        private string testCameraToggleText = "Unknown";
        private bool isTeamsDryRun;
        private string teamsModeText = "Live Teams";
        private string detailText =
            "Open Laptop Companion on the X3 Home screen, then pair/connect over BLE once the firmware service is wired in.";

        public MainWindow()
        {
            InitializeComponent();
            DataContext = this;
            HostLog.Write("Main window created.");
            connectionService.StatusChanged += OnConnectionStatusChanged;
            connectionService.ButtonEventReceived += OnButtonEventReceived;
            statusTimer.Interval = TimeSpan.FromSeconds(2);
            statusTimer.Tick += OnStatusTimerTick;
            Loaded += OnLoaded;
            Closing += OnClosing;
        }

        public event PropertyChangedEventHandler PropertyChanged;

        public string ConnectionText
        {
            get { return connectionText; }
            private set { SetField(ref connectionText, value, nameof(ConnectionText)); }
        }

        public string TeamsText
        {
            get { return teamsText; }
            private set { SetField(ref teamsText, value, nameof(TeamsText)); }
        }

        public string MicrophoneText
        {
            get { return microphoneText; }
            private set { SetField(ref microphoneText, value, nameof(MicrophoneText)); }
        }

        public string CameraText
        {
            get { return cameraText; }
            private set { SetField(ref cameraText, value, nameof(CameraText)); }
        }

        public bool IsTestMode
        {
            get { return isTestMode; }
            set
            {
                if (SetField(ref isTestMode, value, nameof(IsTestMode)))
                {
                    ApplyTestMode();
                }
            }
        }

        public string TestModeText
        {
            get { return testModeText; }
            private set { SetField(ref testModeText, value, nameof(TestModeText)); }
        }

        public bool TestTeamsDetected
        {
            get { return testTeamsDetected; }
            set
            {
                if (SetField(ref testTeamsDetected, value, nameof(TestTeamsDetected)))
                {
                    TestTeamsToggleText = testTeamsDetected ? "Detected" : "Not detected";
                    OnTestStatusChanged("teams");
                }
            }
        }

        public string TestMessage
        {
            get { return testMessage; }
            set
            {
                if (SetField(ref testMessage, value, nameof(TestMessage)) && IsTestMode)
                {
                    DetailText = "Test message updated. Press Send Test Status to push it to the X3.";
                }
            }
        }

        public string TestTeamsToggleText
        {
            get { return testTeamsToggleText; }
            private set { SetField(ref testTeamsToggleText, value, nameof(TestTeamsToggleText)); }
        }

        public string TestMicrophoneToggleText
        {
            get { return testMicrophoneToggleText; }
            private set { SetField(ref testMicrophoneToggleText, value, nameof(TestMicrophoneToggleText)); }
        }

        public string TestCameraToggleText
        {
            get { return testCameraToggleText; }
            private set { SetField(ref testCameraToggleText, value, nameof(TestCameraToggleText)); }
        }

        public bool IsTeamsDryRun
        {
            get { return isTeamsDryRun; }
            set
            {
                if (SetField(ref isTeamsDryRun, value, nameof(IsTeamsDryRun)))
                {
                    ApplyTeamsDryRun();
                }
            }
        }

        public string TeamsModeText
        {
            get { return teamsModeText; }
            private set { SetField(ref teamsModeText, value, nameof(TeamsModeText)); }
        }

        public string DetailText
        {
            get { return detailText; }
            private set { SetField(ref detailText, value, nameof(DetailText)); }
        }

        private bool SetField(ref string field, string value, string propertyName)
        {
            if (field == value)
            {
                return false;
            }

            field = value;
            if (PropertyChanged != null)
            {
                PropertyChanged(this, new PropertyChangedEventArgs(propertyName));
            }

            return true;
        }

        private bool SetField(ref bool field, bool value, string propertyName)
        {
            if (field == value)
            {
                return false;
            }

            field = value;
            if (PropertyChanged != null)
            {
                PropertyChanged(this, new PropertyChangedEventArgs(propertyName));
            }

            return true;
        }
    }
}
