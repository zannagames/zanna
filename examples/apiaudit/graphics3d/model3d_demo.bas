DIM path AS STRING
DIM scene AS OBJECT
DIM parent AS OBJECT
DIM child AS OBJECT
DIM mesh AS OBJECT
DIM mat AS OBJECT
DIM model AS OBJECT
DIM loadResult AS OBJECT
DIM inst AS OBJECT
DIM instScene AS OBJECT
DIM node AS OBJECT
DIM nodeOption AS OBJECT
DIM pos AS OBJECT

PRINT "=== SceneAsset Demo ==="

    path = "scene_asset_demo.vscn"
scene = Zanna.Graphics3D.SceneGraph.New()
parent = Zanna.Graphics3D.SceneNode.New()
child = Zanna.Graphics3D.SceneNode.New()
mesh = Zanna.Graphics3D.Mesh3D.Box(1.0, 2.0, 3.0)
mat = Zanna.Graphics3D.Material3D.FromColor(0.2, 0.4, 0.8)

Zanna.Graphics3D.SceneNode.set_Name(parent, "parent")
Zanna.Graphics3D.SceneNode.set_Name(child, "child")
Zanna.Graphics3D.SceneNode.SetPosition(parent, 1.0, 2.0, 3.0)
Zanna.Graphics3D.SceneNode.SetPosition(child, 0.0, 5.0, 0.0)
parent.Mesh = mesh
parent.Material = mat
child.Mesh = mesh
child.Material = mat
Zanna.Graphics3D.SceneNode.AddChild(parent, child)
Zanna.Graphics3D.SceneGraph.Add(scene, parent)
Zanna.Graphics3D.SceneGraph.Save(scene, path)

loadResult = Zanna.Graphics3D.SceneAsset.LoadResult(path)
model = Zanna.Result.Unwrap(loadResult)
PRINT "MeshCount = "; Zanna.Graphics3D.SceneAsset.get_MeshCount(model)
PRINT "MaterialCount = "; Zanna.Graphics3D.SceneAsset.get_MaterialCount(model)
PRINT "NodeCount = "; Zanna.Graphics3D.SceneAsset.get_NodeCount(model)

nodeOption = Zanna.Graphics3D.SceneAsset.FindNode(model, "child")
node = Zanna.Option.Unwrap(nodeOption)
pos = Zanna.Graphics3D.SceneNode.get_Position(node)
PRINT "Template child Y = "; Zanna.Math.Vec3.get_Y(pos)

inst = Zanna.Graphics3D.SceneAsset.Instantiate(model)
nodeOption = Zanna.Graphics3D.SceneNode.Find(inst, "child")
node = Zanna.Option.Unwrap(nodeOption)
Zanna.Graphics3D.SceneNode.SetPosition(node, 9.0, 9.0, 9.0)
pos = Zanna.Graphics3D.SceneNode.get_Position(node)
PRINT "Instance child Y = "; Zanna.Math.Vec3.get_Y(pos)

instScene = Zanna.Graphics3D.SceneAsset.InstantiateScene(model)
PRINT "Scene node count = "; Zanna.Graphics3D.SceneGraph.get_NodeCount(instScene)
