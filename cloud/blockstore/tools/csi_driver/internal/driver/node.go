package driver

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"

	"github.com/container-storage-interface/spec/lib/go/csi"
	nbsapi "github.com/ydb-platform/nbs/cloud/blockstore/public/api/protos"
	nbsclient "github.com/ydb-platform/nbs/cloud/blockstore/public/sdk/go/client"
	nfsapi "github.com/ydb-platform/nbs/cloud/filestore/public/api/protos"
	nfsclient "github.com/ydb-platform/nbs/cloud/filestore/public/sdk/go/client"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"k8s.io/mount-utils"
)

////////////////////////////////////////////////////////////////////////////////

const topologyNodeKey = "topology.nbs.csi/node"

const nbsSocketName = "nbs.sock"
const nfsSocketName = "nfs.sock"

const vhostIpc = nbsapi.EClientIpcType_IPC_VHOST
const nbdIpc = nbsapi.EClientIpcType_IPC_NBD

var capabilities = []*csi.NodeServiceCapability{
	&csi.NodeServiceCapability{
		Type: &csi.NodeServiceCapability_Rpc{
			Rpc: &csi.NodeServiceCapability_RPC{
				Type: csi.NodeServiceCapability_RPC_STAGE_UNSTAGE_VOLUME,
			},
		},
	},
}

////////////////////////////////////////////////////////////////////////////////

type nodeService struct {
	csi.NodeServer

	nodeID        string
	clientID      string
	vmMode        bool
	nbsSocketsDir string
	podSocketsDir string
	nbsClient     nbsclient.ClientIface
	nfsClient     nfsclient.EndpointClientIface

	mounter mount.Interface
}

func newNodeService(
	nodeID string,
	clientID string,
	vmMode bool,
	nbsSocketsDir string,
	podSocketsDir string,
	nbsClient nbsclient.ClientIface,
	nfsClient nfsclient.EndpointClientIface) csi.NodeServer {

	return &nodeService{
		nodeID:        nodeID,
		clientID:      clientID,
		vmMode:        vmMode,
		nbsSocketsDir: nbsSocketsDir,
		podSocketsDir: podSocketsDir,
		nbsClient:     nbsClient,
		nfsClient:     nfsClient,
		mounter:       mount.New(""),
	}
}

func (s *nodeService) NodeStageVolume(
	ctx context.Context,
	req *csi.NodeStageVolumeRequest) (*csi.NodeStageVolumeResponse, error) {

	log.Printf("csi.NodeStageVolumeRequest: %+v", req)

	if req.VolumeId == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeId missing in NodeStageVolumeRequest")
	}
	if req.StagingTargetPath == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"StagingTargetPath missing in NodeStageVolumeRequest")
	}
	if req.VolumeCapability == nil {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeCapability missing in NodeStageVolumeRequest")
	}

	return &csi.NodeStageVolumeResponse{}, nil
}

func (s *nodeService) NodeUnstageVolume(
	ctx context.Context,
	req *csi.NodeUnstageVolumeRequest) (*csi.NodeUnstageVolumeResponse, error) {

	log.Printf("csi.NodeUnstageVolumeRequest: %+v", req)

	if req.VolumeId == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeId missing in NodeUnstageVolumeRequest")
	}
	if req.StagingTargetPath == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"StagingTargetPath missing in NodeUnstageVolumeRequest")
	}

	return &csi.NodeUnstageVolumeResponse{}, nil
}

func (s *nodeService) NodePublishVolume(
	ctx context.Context,
	req *csi.NodePublishVolumeRequest) (*csi.NodePublishVolumeResponse, error) {

	log.Printf("csi.NodePublishVolumeRequest: %+v", req)

	if req.VolumeId == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeId missing in NodePublishVolumeRequest")
	}
	if req.StagingTargetPath == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"StagingTargetPath missing in NodePublishVolumeRequest")
	}
	if req.TargetPath == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"TargetPath missing in NodePublishVolumeRequest")
	}
	if req.VolumeCapability == nil {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeCapability missing in NodePublishVolumeRequest")
	}
	if req.VolumeContext == nil {
		return nil, status.Error(
			codes.InvalidArgument,
			"VolumeContext missing in NodePublishVolumeRequest")
	}

	if getPodId(req) == "" {
		return nil, status.Errorf(codes.Internal,
			"podUID missing in NodePublishVolumeRequest.VolumeContext")
	}

	var err error
	nfsBackend := (req.VolumeContext["backend"] == "nfs")

	switch req.VolumeCapability.GetAccessType().(type) {
	case *csi.VolumeCapability_Mount:
		if s.vmMode {
			if nfsBackend {
				err = s.nodePublishFileStoreAsVhostSocket(ctx, req)
			} else {
				err = s.nodePublishDiskAsVhostSocket(ctx, req)
			}
		} else {
			if nfsBackend {
				err = status.Error(codes.InvalidArgument,
					"FileStore can't be mounted to container as a filesystem")
			} else {
				err = s.nodePublishDiskAsFilesystem(ctx, req)
			}
		}
	case *csi.VolumeCapability_Block:
		if nfsBackend {
			err = status.Error(codes.InvalidArgument,
				"'Block' volume mode is not supported for nfs backend")
		} else {
			err = s.nodePublishDiskAsBlockDevice(ctx, req)
		}
	default:
		err = status.Error(codes.InvalidArgument, "Unknown access type")
	}

	if err != nil {
		return nil, err
	}

	return &csi.NodePublishVolumeResponse{}, nil
}

func (s *nodeService) NodeUnpublishVolume(
	ctx context.Context,
	req *csi.NodeUnpublishVolumeRequest,
) (*csi.NodeUnpublishVolumeResponse, error) {

	log.Printf("csi.NodeUnpublishVolumeRequest: %+v", req)

	if req.VolumeId == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"Volume ID missing in NodeUnpublishVolumeRequest")
	}
	if req.TargetPath == "" {
		return nil, status.Error(
			codes.InvalidArgument,
			"Target Path missing in NodeUnpublishVolumeRequest")
	}

	if err := s.nodeUnpublishVolume(ctx, req); err != nil {
		return nil, err
	}

	return &csi.NodeUnpublishVolumeResponse{}, nil
}

func (s *nodeService) NodeGetCapabilities(
	ctx context.Context,
	req *csi.NodeGetCapabilitiesRequest,
) (*csi.NodeGetCapabilitiesResponse, error) {

	return &csi.NodeGetCapabilitiesResponse{
		Capabilities: capabilities,
	}, nil
}

func (s *nodeService) NodeGetInfo(
	ctx context.Context,
	req *csi.NodeGetInfoRequest) (*csi.NodeGetInfoResponse, error) {

	return &csi.NodeGetInfoResponse{
		NodeId: s.nodeID,
		AccessibleTopology: &csi.Topology{
			Segments: map[string]string{topologyNodeKey: s.nodeID},
		},
	}, nil
}

func (s *nodeService) nodePublishDiskAsVhostSocket(
	ctx context.Context,
	req *csi.NodePublishVolumeRequest) error {

	_, err := s.startNbsEndpoint(ctx, getPodId(req), req.VolumeId, vhostIpc)
	if err != nil {
		return status.Errorf(codes.Internal,
			"Failed to start NBS endpoint: %+v", err)
	}

	return s.mountSocketDir(req)
}

func (s *nodeService) nodePublishDiskAsFilesystem(
	ctx context.Context,
	req *csi.NodePublishVolumeRequest) error {

	resp, err := s.startNbsEndpoint(ctx, getPodId(req), req.VolumeId, nbdIpc)
	if err != nil {
		return status.Errorf(codes.Internal,
			"Failed to start NBS endpoint: %+v", err)
	}

	if resp.NbdDeviceFile == "" {
		return status.Error(codes.Internal, "NbdDeviceFile shouldn't be empty")
	}

	log.Printf("Endpoint started with device file: %q", resp.NbdDeviceFile)

	fsType := "ext4"
	mnt := req.VolumeCapability.GetMount()
	if mnt != nil && mnt.FsType != "" {
		fsType = mnt.FsType
	}

	if err := makeFilesystemIfNeeded(resp.NbdDeviceFile, fsType); err != nil {
		return err
	}

	if err := os.MkdirAll(req.TargetPath, 0755); err != nil {
		return fmt.Errorf("failed to create target directory: %v", err)
	}

	mountOptions := []string{}
	if mnt != nil {
		for _, flag := range mnt.MountFlags {
			mountOptions = append(mountOptions, flag)
		}
	}
	return s.mounter.Mount(resp.NbdDeviceFile, req.TargetPath, fsType, mountOptions)
}

func (s *nodeService) nodePublishDiskAsBlockDevice(
	ctx context.Context,
	req *csi.NodePublishVolumeRequest) error {

	resp, err := s.startNbsEndpoint(ctx, getPodId(req), req.VolumeId, nbdIpc)
	if err != nil {
		return status.Errorf(codes.Internal,
			"Failed to start NBS endpoint: %+v", err)
	}

	if resp.NbdDeviceFile == "" {
		return status.Error(codes.Internal, "NbdDeviceFile shouldn't be empty")
	}

	log.Printf("Endpoint started with device file: %q", resp.NbdDeviceFile)
	return s.mountBlockDevice(resp.NbdDeviceFile, req.TargetPath)
}

func (s *nodeService) startNbsEndpoint(
	ctx context.Context,
	podId string,
	volumeId string,
	ipcType nbsapi.EClientIpcType) (*nbsapi.TStartEndpointResponse, error) {

	endpointDir := filepath.Join(s.podSocketsDir, podId, volumeId)
	if err := os.MkdirAll(endpointDir, 0777); err != nil {
		return nil, err
	}

	socketPath := filepath.Join(s.nbsSocketsDir, podId, volumeId, nbsSocketName)
	hostType := nbsapi.EHostType_HOST_TYPE_DEFAULT
	return s.nbsClient.StartEndpoint(ctx, &nbsapi.TStartEndpointRequest{
		UnixSocketPath:   socketPath,
		DiskId:           volumeId,
		ClientId:         s.clientID,
		DeviceName:       volumeId,
		IpcType:          ipcType,
		VhostQueuesCount: 8,
		VolumeAccessMode: nbsapi.EVolumeAccessMode_VOLUME_ACCESS_READ_WRITE,
		VolumeMountMode:  nbsapi.EVolumeMountMode_VOLUME_MOUNT_REMOTE,
		Persistent:       true,
		NbdDevice: &nbsapi.TStartEndpointRequest_UseFreeNbdDeviceFile{
			ipcType == nbdIpc,
		},
		ClientProfile: &nbsapi.TClientProfile{
			HostType: &hostType,
		},
	})
}

func (s *nodeService) nodePublishFileStoreAsVhostSocket(
	ctx context.Context,
	req *csi.NodePublishVolumeRequest) error {

	endpointDir := filepath.Join(s.podSocketsDir, getPodId(req), req.VolumeId)
	if err := os.MkdirAll(endpointDir, 0777); err != nil {
		return err
	}

	if s.nfsClient == nil {
		return status.Errorf(codes.Internal, "NFS client wasn't created")
	}

	socketPath := filepath.Join(s.nbsSocketsDir, getPodId(req), req.VolumeId, nfsSocketName)
	_, err := s.nfsClient.StartEndpoint(ctx, &nfsapi.TStartEndpointRequest{
		Endpoint: &nfsapi.TEndpointConfig{
			SocketPath:       socketPath,
			FileSystemId:     req.VolumeId,
			ClientId:         s.clientID,
			VhostQueuesCount: 8,
			Persistent:       true,
		},
	})
	if err != nil {
		return status.Errorf(codes.Internal,
			"Failed to start NFS endpoint: %+v", err)
	}

	return s.mountSocketDir(req)
}

func (s *nodeService) nodeUnpublishVolume(
	ctx context.Context,
	req *csi.NodeUnpublishVolumeRequest) error {

	if err := mount.CleanupMountPoint(req.TargetPath, s.mounter, true); err != nil {
		return err
	}

	// no other way to get podId from NodeUnpublishVolumeRequest
	podId, _, err := parseTargetPath(req.TargetPath)
	if err != nil {
		return err
	}

	podSocketDir := filepath.Join(s.podSocketsDir, podId, req.VolumeId)
	nodeSocketDir := filepath.Join(s.nbsSocketsDir, podId, req.VolumeId)

	_, err = os.Stat(filepath.Join(podSocketDir, nbsSocketName))
	if os.IsExist(err) {
		log.Printf("stop nbs endpoint, pod: %q, volume: %q", podId, req.VolumeId)
		_, err := s.nbsClient.StopEndpoint(ctx, &nbsapi.TStopEndpointRequest{
			UnixSocketPath: filepath.Join(nodeSocketDir, nbsSocketName),
		})
		if err != nil {
			return status.Errorf(codes.Internal,
				"Failed to stop nbs endpoint: %+v", err)
		}
	}

	_, err = os.Stat(filepath.Join(podSocketDir, nfsSocketName))
	if os.IsExist(err) {
		log.Printf("stop nfs endpoint, pod: %q, volume: %q", podId, req.VolumeId)
		_, err = s.nfsClient.StopEndpoint(ctx, &nfsapi.TStopEndpointRequest{
			SocketPath: filepath.Join(nodeSocketDir, nfsSocketName),
		})
		if err != nil {
			return status.Errorf(codes.Internal,
				"Failed to stop nfs endpoint: %+v", err)
		}
	}

	return os.RemoveAll(podSocketDir)
}

func (s *nodeService) mountSocketDir(req *csi.NodePublishVolumeRequest) error {

	endpointDir := filepath.Join(s.podSocketsDir, getPodId(req), req.VolumeId)

	// https://kubevirt.io/user-guide/virtual_machines/disks_and_volumes/#persistentvolumeclaim
	// "If the disk.img image file has not been created manually before starting a VM
	// then it will be created automatically with the PersistentVolumeClaim size."
	// So, let's create an empty disk.img to avoid automatic creation and save disk space.
	diskImgPath := filepath.Join(endpointDir, "disk.img")
	file, err := os.OpenFile(diskImgPath, os.O_CREATE, 0660)
	if err != nil {
		return status.Errorf(codes.Internal, "Failed to create disk.img: %+v", err)
	}
	file.Close()

	if err := os.MkdirAll(req.TargetPath, 0755); err != nil {
		return fmt.Errorf("failed to create target directory: %v", err)
	}

	mountOptions := []string{"bind"}
	mnt := req.VolumeCapability.GetMount()
	if mnt != nil {
		for _, flag := range mnt.MountFlags {
			mountOptions = append(mountOptions, flag)
		}
	}
	return s.mounter.Mount(endpointDir, req.TargetPath, "", mountOptions)
}

func (s *nodeService) mountBlockDevice(source string, target string) error {
	err := os.MkdirAll(filepath.Dir(target), 0750)
	if err != nil {
		return fmt.Errorf("failed to create target directory: %v", err)
	}

	file, err := os.OpenFile(target, os.O_CREATE, 0660)
	if err != nil {
		return fmt.Errorf("failed to create target file: %v", err)
	}
	file.Close()

	mountOptions := []string{"bind"}
	return s.mounter.Mount(source, target, "", mountOptions)
}

func makeFilesystemIfNeeded(deviceName, fsType string) error {
	if _, err := exec.LookPath("blkid"); err != nil {
		return fmt.Errorf("failed to find 'blkid' tool: %v", err)
	}

	if _, err := os.Stat(deviceName); os.IsNotExist(err) {
		return fmt.Errorf("failed to find device %q: %v", deviceName, err)
	}

	out, err := exec.Command("blkid", deviceName).CombinedOutput()
	if err == nil && string(out) != "" {
		log.Printf("filesystem exists: %q", string(out))
		return nil
	}

	log.Printf("making filesystem %q on device %q", fsType, deviceName)

	out, err = exec.Command("mkfs", "-t", fsType, deviceName).CombinedOutput()
	if err != nil {
		return fmt.Errorf("failed to make filesystem: %v, output %q", err, out)
	}

	log.Printf("succeeded making filesystem: %q", out)
	return nil
}

func getPodId(req *csi.NodePublishVolumeRequest) string {
	// another way to get podId is: return req.VolumeContext["csi.storage.k8s.io/pod.uid"]
	podId, _, err := parseTargetPath(req.TargetPath)
	if err != nil {
		return ""
	}

	return podId
}

func parseTargetPath(targetPath string) (string, string, error) {
	re := regexp.MustCompile(`/var/lib/kubelet/pods/([a-z0-9-]+)/volumes/kubernetes.io~csi/([a-z0-9-]+)/mount`)
	matches := re.FindStringSubmatch(targetPath)

	if len(matches) <= 2 {
		return "", "", fmt.Errorf("failed to parse TargetPath: %q", targetPath)
	}

	podID := matches[1]
	volumeID := matches[2]
	return podID, volumeID, nil
}
