Import('env')
from os.path import join
root = join(env['PROJECT_DIR'], 'lib', 'esp_audio_codec')
env.Append(CPPPATH=[join(root, 'include'), join(root, 'include', 'decoder'), join(root, 'include', 'decoder', 'impl')])
env.Append(LIBPATH=[root], LIBS=['esp_audio_codec'])
