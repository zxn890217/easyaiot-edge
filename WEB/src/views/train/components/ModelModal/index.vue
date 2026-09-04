<template>
  <BasicDrawer
    v-bind="$attrs"
    @register="register"
    :title="getTitle"
    width="1400"
    placement="right"
    :showFooter="true"
    :showCancelBtn="false"
    :showOkBtn="false"
    destroy-on-close
  >
    <template #footer>
      <div class="footer-buttons">
        <Button @click="handleCancel">{{ state.isView ? '关闭' : '取消' }}</Button>
        <Button v-if="!state.isView" type="primary" :loading="state.editLoading" @click="handleOk">
          保存
        </Button>
      </div>
    </template>

    <Spin :spinning="state.editLoading">
      <div class="model-drawer-content">
        <BasicForm @register="registerForm" />

        <Divider orientation="left">模型资源</Divider>
        <Form
          :label-col="{ style: { width: '150px' } }"
          :wrapper-col="{ span: 21 }"
          :disabled="state.isView"
          class="resource-form"
        >
          <FormItem label="模型图片" required>
            <Upload
              name="file"
              :action="state.imageUploadUrl"
              :headers="headers"
              :show-upload-list="false"
              accept=".jpg,.jpeg,.png"
              :disabled="state.isView"
              @change="handleImageUpload"
            >
              <Button type="primary" :disabled="state.isView">
                {{ state.isView ? '已上传' : '上传模型图片' }}
              </Button>
            </Upload>
            <div v-if="modelRef.imageUrl" class="preview-wrap">
              <img :src="resolveModelImageDisplayUrl(modelRef.imageUrl)" alt="模型图片" class="preview-image" />
            </div>
          </FormItem>

          <FormItem label="模型文件">
            <Upload
              name="file"
              :action="state.modelUploadUrl"
              :headers="headers"
              :show-upload-list="false"
              accept=".pt,.pth,.h5,.onnx,.rknn"
              :disabled="state.isView"
              :custom-request="handleModelCustomRequest"
            >
              <Button type="primary" :loading="state.modelUploading" :disabled="state.isView">
                {{ state.isView ? '已上传' : '上传模型文件' }}
              </Button>
            </Upload>
            <div v-if="modelRef.filePath" class="form-extra" :title="fileName">
              已上传：{{ fileName }}
            </div>
            <div class="form-hint">支持 .pt / .onnx / .rknn；继续训练需使用 .pt 权重</div>
          </FormItem>

          <FormItem v-if="isRknnPending" label="NPU 配套文件">
            <Upload
              v-if="auxFiles.length > 0 || !modelRef.filePath"
              :show-upload-list="false"
              :multiple="true"
              accept=".names,.json"
              :before-upload="handleAuxBeforeUpload"
              :disabled="state.isView"
            >
              <Button :disabled="state.isView">选择 model.names / model.rknn.json</Button>
            </Upload>
            <div v-if="auxFiles.length > 0" class="form-extra">
              <span v-for="(f, idx) in auxFiles" :key="f.name" style="margin-right: 12px">
                {{ f.name }}
                <a @click="removeAuxFile(idx)">删除</a>
              </span>
            </div>
            <div v-if="modelRef.filePath" style="margin-top: 6px">
              <Upload
                :show-upload-list="false"
                :multiple="true"
                accept=".names,.json"
                :before-upload="handleAuxUploadForExisting"
                :disabled="state.isView"
              >
                <Button :loading="state.auxUploading" :disabled="state.isView">补充上传配套文件</Button>
              </Upload>
              <div v-if="uploadedAuxNames.length > 0" class="form-extra">
                已上传配套文件：{{ uploadedAuxNames.join('、') }}
              </div>
            </div>
            <div class="form-hint">
              RK3588 NPU 推理需上传 x86 转换产物三件套：model.rknn（上方主文件）+
              model.names、model.rknn.json（此处配套文件，文件名需与主文件同名）
            </div>
          </FormItem>

          <FormItem label="检测类别">
            <div class="class-tags-panel">
              <div v-if="state.classNames.length > 0" class="class-tag-list">
                <Tag v-for="name in state.classNames" :key="name" color="blue">{{ name }}</Tag>
              </div>
              <div v-if="state.classNames.length > 0" class="form-hint">
                共 {{ state.classNames.length }} 个检测类别
              </div>
              <span v-else class="form-hint">
                上传 .pt / .onnx 权重或 .rknn（配套 .names）后将自动识别检测类别
              </span>
            </div>
          </FormItem>
        </Form>
      </div>
    </Spin>
  </BasicDrawer>
</template>

<script lang="ts" setup>
import { computed, reactive, ref } from 'vue';
import { BasicDrawer, useDrawerInner } from '@/components/Drawer';
import { BasicForm, useForm } from '@/components/Form';
import { Form, FormItem, Spin, Upload, Divider, Tag } from 'ant-design-vue';
import { useMessage } from '@/hooks/web/useMessage';
import { useUserStoreWithOut } from '@/store/modules/user';
import { useGlobSetting } from '@/hooks/setting';
import { createModel, updateModel, getModelClasses, parseModelClassPayload } from '@/api/device/model';
import { normalizeModelVersion } from '../../utils/modelVersionUtils';
import { resolveModelImageDisplayUrl } from '@/utils/alertMinioImage';
import { Button } from '@/components/Button';

defineOptions({ name: 'ModelDrawer' });

const { createMessage } = useMessage();

const userStore = useUserStoreWithOut();
const token = userStore.getAccessToken;
const headers = ref({ 'X-Authorization': `Bearer ${token}` });
const { uploadUrl } = useGlobSetting();

const state = reactive({
  modelUploadUrl: `${uploadUrl}/model/upload`,
  imageUploadUrl: `${uploadUrl}/model/image_upload`,
  isEdit: false,
  isView: false,
  editLoading: false,
  modelUploading: false,
  auxUploading: false,
  classNames: [] as string[],
});

const modelRef = reactive({
  id: null as number | null,
  filePath: '',
  imageUrl: '',
});

// .rknn 配套文件（model.names / model.rknn.json）：与主文件一次请求上传，后端存同 prefix 同 stem
const auxFiles = ref<File[]>([]);
// 主文件已上传后补传的配套文件展示名（后端按主文件 stem 重命名存库）
const uploadedAuxNames = ref<string[]>([]);
// 主文件选了 .rknn 但尚未上传成功时，也要展示配套文件区
const pendingRknn = ref(false);

function getPathExt(path: string) {
  const clean = (path || '').split('?')[0].toLowerCase();
  const dot = clean.lastIndexOf('.');
  return dot >= 0 ? clean.slice(dot) : '';
}

const isRknnPending = computed(
  () => pendingRknn.value || getPathExt(modelRef.filePath) === '.rknn',
);

function isRknnAuxName(name: string) {
  const lower = (name || '').toLowerCase();
  return lower.endsWith('.names') || lower.endsWith('.rknn.json');
}

function resetAuxState() {
  auxFiles.value = [];
  uploadedAuxNames.value = [];
  pendingRknn.value = false;
}

const getTitle = computed(() => (state.isEdit ? '编辑模型' : state.isView ? '查看模型' : '新增模型'));

const fileName = computed(() => {
  const path = modelRef.filePath || '';
  if (!path) return '';
  return path.split('/').pop()?.split('?')[0] || path;
});

const emits = defineEmits(['success', 'register']);

const [registerForm, { setFieldsValue, validate, resetFields, setProps }] = useForm({
  labelWidth: 150,
  baseColProps: { span: 24 },
  showActionButtonGroup: false,
  schemas: [
    {
      field: 'name',
      label: '模型名称',
      component: 'Input',
      required: true,
      componentProps: { placeholder: '请输入模型名称' },
    },
    {
      field: 'version',
      label: '模型版本',
      component: 'Input',
      required: true,
      defaultValue: '1.0.0',
      componentProps: { placeholder: '例如：1.0.0' },
    },
    {
      field: 'description',
      label: '模型描述',
      component: 'InputTextArea',
      componentProps: { placeholder: '请输入模型描述', rows: 4 },
    },
    {
      field: 'status',
      label: '状态',
      component: 'Select',
      required: true,
      componentProps: {
        placeholder: '请选择状态',
        options: [
          { value: 0, label: '未部署' },
          { value: 1, label: '已部署' },
          { value: 3, label: '已下线' },
        ],
      },
    },
  ],
});

const [register, { closeDrawer }] = useDrawerInner((data) => {
  const { isEdit, isView, record } = data || {};
  state.isEdit = !!isEdit;
  state.isView = !!isView;

  setProps({ disabled: state.isView });

  if (state.isEdit || state.isView) {
    modelEdit(record);
  } else {
    resetFormState();
  }
});

function resetFormState() {
  resetFields();
  modelRef.id = null;
  modelRef.filePath = '';
  modelRef.imageUrl = '';
  state.classNames = [];
  resetAuxState();
}

function applyClassNames(classNames: string[]) {
  state.classNames = Array.isArray(classNames) ? [...classNames] : [];
}

async function loadClassNamesForRecord(record: any) {
  const fromRecord = parseModelClassPayload(record);
  if (fromRecord.classNames.length > 0) {
    applyClassNames(fromRecord.classNames);
    return;
  }
  if (!record?.id) return;
  try {
    const resp = await getModelClasses(record.id);
    applyClassNames(parseModelClassPayload(resp).classNames);
  } catch (error) {
    console.warn('加载检测类别失败', error);
  }
}

async function modelEdit(record: any) {
  try {
    state.editLoading = true;
    modelRef.id = record.id ?? null;
    const rknnPath = record.rknn_model_path || '';
    // rknn 主模型优先展示（NPU 场景），否则回退 pt/onnx
    modelRef.filePath =
      rknnPath || record.filePath || record.model_path || record.onnx_model_path || '';
    resetAuxState();
    modelRef.imageUrl = record.imageUrl ?? record.image_url ?? '';
    const s = record.status;
    await setFieldsValue({
      name: record.name ?? '',
      version: normalizeModelVersion(record.version),
      description: record.description ?? '',
      status: s === '' || s === undefined || s === null ? 0 : Number(s),
    });
    await loadClassNamesForRecord(record);
  } catch (error) {
    console.error(error);
    createMessage.error('加载模型信息失败');
  } finally {
    state.editLoading = false;
  }
}

function handleCancel() {
  resetFormState();
  closeDrawer();
}

function handleAuxBeforeUpload(file: File) {
  const name = (file.name || '').toLowerCase();
  if (!name.endsWith('.names') && !name.endsWith('.rknn.json')) {
    createMessage.warning('配套文件仅支持 .names / .rknn.json');
    return false;
  }
  if (!auxFiles.value.some((f) => f.name === file.name)) {
    auxFiles.value.push(file);
  }
  return false;
}

function removeAuxFile(index: number) {
  auxFiles.value.splice(index, 1);
}

// 主文件已上传后补传配套文件：走 /model/upload_aux，后端按主文件 stem 重命名存同 prefix
function handleAuxUploadForExisting(file: File) {
  const name = (file.name || '').toLowerCase();
  if (!isRknnAuxName(name)) {
    createMessage.warning('配套文件仅支持 .names / .rknn.json');
    return false;
  }
  const formData = new FormData();
  formData.append('file', file);
  formData.append('primary_url', modelRef.filePath);
  state.auxUploading = true;
  fetch(state.modelAuxUploadUrl, {
    method: 'POST',
    headers: { ...headers.value },
    body: formData,
  })
    .then((resp) => resp.json())
    .then((response) => {
      if (response && response.code === 0) {
        const display = response.data?.fileName || file.name;
        if (!uploadedAuxNames.value.includes(display)) {
          uploadedAuxNames.value.push(display);
        }
        const classNames = parseModelClassPayload(response.data || {}).classNames;
        if (classNames.length > 0) {
          applyClassNames(classNames);
        }
        createMessage.success(`配套文件 ${display} 上传成功`);
      } else {
        createMessage.error(response?.msg || '配套文件上传失败');
      }
    })
    .catch((error) => {
      createMessage.error(error?.message || '配套文件上传失败');
    })
    .finally(() => {
      state.auxUploading = false;
    });
  return false;
}

async function handleModelCustomRequest(request: any) {
  const file: File = request.file;
  const ext = `.${(file.name || '').split('.').pop()?.toLowerCase() || ''}`;
  pendingRknn.value = ext === '.rknn';
  const formData = new FormData();
  formData.append('file', file);
  if (ext === '.rknn') {
    auxFiles.value.forEach((aux) => formData.append('aux_files', aux));
  }
  state.modelUploading = true;
  try {
    const resp = await fetch(state.modelUploadUrl, {
      method: 'POST',
      headers: { ...headers.value },
      body: formData,
    });
    const response = await resp.json();
    if (response && response.code === 0) {
      modelRef.filePath = response.data.url;
      applyClassNames(parseModelClassPayload(response.data).classNames);
      if (ext === '.rknn' && response.data.classNames?.length) {
        createMessage.success('模型文件上传成功（检测类别已从 .names 识别）');
      } else if (ext === '.rknn' && auxFiles.value.length === 0) {
        createMessage.warning('模型文件上传成功，但未上传 .names 配套文件，检测类别为空');
      } else {
        createMessage.success('模型文件上传成功');
      }
      if (ext === '.rknn') {
        // 配套文件已随主文件一次请求存库，转入"已上传配套"展示
        uploadedAuxNames.value = auxFiles.value.map((f) => f.name);
        auxFiles.value = [];
      } else {
        resetAuxState();
      }
      request.onSuccess?.(response);
    } else {
      createMessage.error(response?.msg || '文件上传失败');
      request.onError?.(new Error(response?.msg || '文件上传失败'));
    }
  } catch (error: any) {
    createMessage.error(error?.message || '文件上传失败');
    request.onError?.(error);
  } finally {
    state.modelUploading = false;
  }
}

function handleImageUpload(info: any) {
  if (info.file.status === 'done') {
    const response = info.file.response;
    if (response && response.code === 0) {
      modelRef.imageUrl = response.data.url;
      createMessage.success('模型图片上传成功');
    } else {
      createMessage.error(response?.msg || '图片上传失败');
    }
  } else if (info.file.status === 'error') {
    createMessage.error('图片上传失败');
  }
}

async function handleOk() {
  try {
    const values = await validate();
    if (!modelRef.imageUrl) {
      createMessage.warning('请上传模型图片');
      return;
    }
    state.editLoading = true;
    const api = modelRef.id ? updateModel : createModel;
    const payload = {
      id: modelRef.id,
      name: values.name,
      version: normalizeModelVersion(values.version),
      description: values.description,
      status: values.status,
      filePath: modelRef.filePath,
      imageUrl: modelRef.imageUrl,
      classNames: state.classNames,
      selectedClassNames: state.classNames,
    };

    await api(payload);
    createMessage.success('操作成功');
    closeDrawer();
    resetFormState();
    emits('success');
  } catch (error) {
    console.error(error);
  } finally {
    state.editLoading = false;
  }
}
</script>

<style lang="less" scoped>
.model-drawer-content {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.resource-form {
  :deep(.ant-form-item) {
    margin-bottom: 16px;
  }
}

.preview-wrap {
  margin-top: 8px;
}

.preview-image {
  max-height: 120px;
  max-width: 100%;
  border-radius: 4px;
  border: 1px solid #f0f0f0;
  display: block;
}

.form-extra {
  margin-top: 8px;
  color: rgba(0, 0, 0, 0.65);
  font-size: 13px;
  word-break: break-all;
}

.class-tags-panel {
  width: 100%;
}

.class-tag-list {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  max-height: 160px;
  overflow-y: auto;
  padding: 10px 12px;
  border: 1px solid #f0f0f0;
  border-radius: 6px;
  background: #fafafa;

  :deep(.ant-tag) {
    margin: 0;
  }
}

.form-hint {
  margin-top: 4px;
  color: rgba(0, 0, 0, 0.45);
  font-size: 13px;
  line-height: 1.5;
}

.footer-buttons {
  display: flex;
  justify-content: flex-end;
  align-items: center;
  gap: 8px;
}
</style>
