#include "browser.h"
#include "value_text.h"
#include "test_support.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/js/json.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/cube.h"

using namespace vela;

namespace {

JsValue Parse(const std::string &text, const SdfValueTypeName &type, std::string *error = nullptr) {
    JsValue value;
    std::string message;
    if (!ParseEditedText(text, type, &value, &message)) {
        if (error) *error = message;
        return JsValue();
    }
    if (error) error->clear();
    return value;
}

void TestFormatAndParse() {
    const auto &T = SdfValueTypeNames;
    CHECK(IsTextEditable(T->Bool) && IsTextEditable(T->Float3) && IsTextEditable(T->Asset));
    CHECK(!IsTextEditable(T->FloatArray) && !IsTextEditable(T->Matrix4d) &&
          !IsTextEditable(T->Quatf) && !IsTextEditable(T->Half));

    CHECK(FormatForEditing(VtValue(true)) == "true");
    CHECK(FormatForEditing(VtValue(0.1)) == "0.1");
    CHECK(FormatForEditing(VtValue(0.1f)) == "0.1");     // shortest float text
    CHECK(FormatForEditing(VtValue(1.0 / 3.0)) == "0.3333333333333333");
    CHECK(FormatForEditing(VtValue(GfVec3f(1, 2.5f, -3))) == "1, 2.5, -3");
    CHECK(FormatForEditing(VtValue(TfToken("catmullClark"))) == "catmullClark");
    CHECK(FormatForEditing(VtValue(SdfAssetPath("tex.png"))) == "tex.png");
    CHECK(FormatForEditing(VtValue(GfVec2i(4, 5))) == "4, 5");
    CHECK(FormatForEditing(VtValue(GfVec3f(0.1f, 0, 0))) == "0.1, 0, 0");

    std::string error;
    CHECK(Parse(" Yes ", T->Bool) == JsValue(true));
    CHECK(Parse("0", T->Bool) == JsValue(false));
    Parse("maybe", T->Bool, &error);
    CHECK(error.find("Not true or false") == 0);
    CHECK(Parse(" -12 ", T->Int) == JsValue(int64_t(-12)));
    Parse("1.5", T->Int, &error);
    CHECK(error == "Not an integer: 1.5");
    Parse("-1", T->UInt, &error);
    CHECK(error.find("non-negative") != std::string::npos);
    CHECK(Parse("2.5e1", T->Double) == JsValue(25.0));
    Parse("inf", T->Float, &error);
    CHECK(error == "Not a number: inf");
    Parse("", T->Float, &error);
    CHECK(error == "A number is required");
    CHECK(Parse("  spaces kept ", T->String) == JsValue("  spaces kept "));
    CHECK(Parse("(1, 2, 3)", T->Float3) ==
          JsValue(JsArray{JsValue(1.0), JsValue(2.0), JsValue(3.0)}));
    CHECK(Parse("1 2 3", T->Point3f).GetJsArray().size() == 3);
    Parse("1, 2", T->Float3, &error);
    CHECK(error.find("Expected 3 numbers") == 0);
    Parse("1, x, 3", T->Float3, &error);
    CHECK(error == "Not a number: x");
    CHECK(Parse("4, 5", T->Int2) == JsValue(JsArray{JsValue(int64_t(4)), JsValue(int64_t(5))}));
    Parse("1, 2", T->FloatArray, &error);
    CHECK(error.find("cannot be edited as text") != std::string::npos);
}

void TestEditing() {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(10);
    UsdGeomCube cube = UsdGeomCube::Define(stage, SdfPath("/Cube"));
    cube.CreateSizeAttr(VtValue(2.0));
    cube.CreateDisplayColorAttr();
    auto color = cube.GetPrim().CreateAttribute(TfToken("tint"), SdfValueTypeNames->Color3f);
    color.Set(GfVec3f(1, 0, 0), UsdTimeCode(1));
    color.Set(GfVec3f(0, 0, 1), UsdTimeCode(10));
    auto shape = cube.GetPrim().GetVariantSets().AddVariantSet("shape");
    shape.AddVariant("round");
    shape.AddVariant("square");
    Document d(stage);
    Browser b(d);
    const SdfPath path("/Cube");

    std::string text;
    CHECK(b.EditableText(path, "size", &text) && text == "2");
    CHECK(!b.EditableText(path, "extent", &text));            // an array
    CHECK(!b.EditableText(path, "primvars:displayColor", &text));
    CHECK(!b.EditableText(path, "missing", &text));

    // A default value: one undo step.
    CHECK(b.EditAttribute(path, "size", "3.5").ok);
    double size = 0;
    cube.GetSizeAttr().Get(&size);
    CHECK(size == 3.5);
    CHECK(d.GetUndoStack().GetUndoName() == "Set Attribute");
    CHECK(d.GetUndoStack().Undo());
    cube.GetSizeAttr().Get(&size);
    CHECK(size == 2.0);

    // An animated attribute: edited at the current frame only.
    d.SetTime(UsdTimeCode(5));
    CHECK(b.EditableText(path, "tint", &text));
    CHECK(b.EditAttribute(path, "tint", "0, 1, 0").ok);
    GfVec3f at5, at1;
    color.Get(&at5, UsdTimeCode(5));
    color.Get(&at1, UsdTimeCode(1));
    CHECK(at5 == GfVec3f(0, 1, 0));
    CHECK(at1 == GfVec3f(1, 0, 0));
    CHECK(color.GetNumTimeSamples() == 3);
    VtValue defaultValue;
    CHECK(!color.Get(&defaultValue, UsdTimeCode::Default()));

    // Bad text: an error and no edit.
    ActionResult bad = b.EditAttribute(path, "size", "big");
    CHECK(!bad.ok && bad.error == "Not a number: big");
    CHECK(b.EditAttribute(path, "extent", "1").error.find("cannot be edited") != std::string::npos);

    // A read-only edit target makes nothing editable.
    stage->GetRootLayer()->SetPermissionToEdit(false);
    CHECK(!b.EditableText(path, "size", &text));
    stage->GetRootLayer()->SetPermissionToEdit(true);

    CHECK((b.VariantChoices(path, "shape") == std::vector<std::string>{"round", "square"}));
    CHECK(b.VariantChoices(path, "missing").empty());
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestFormatAndParse();
    TestEditing();
    return test::Finish("value text");
}
