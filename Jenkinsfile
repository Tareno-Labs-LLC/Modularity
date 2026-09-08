pipeline {
    agent any

    stages {
        stage('Install Dependencies') {
            steps {
                sh '''
                    sudo apt update
                    sudo apt install -y \
                        cmake \
                        build-essential \
                        zlib1g-dev \
                        libx11-dev \
                        libxrandr-dev \
                        libxinerama-dev \
                        libxcursor-dev \
                        libxi-dev \
                        libgl1-mesa-dev \
                        libegl1-mesa-dev \
                        libwayland-dev \
                        ccache || true
                '''
            }
        }

        stage('Build') {
            steps {
                sh '''
                    chmod +x build.sh
                    ./build.sh
                '''
            }
        }
    }

    post {
        success {
            archiveArtifacts artifacts: 'build/*.tar.gz', fingerprint: true
        }
    }
}
