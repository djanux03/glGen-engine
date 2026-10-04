#include <doctest/doctest.h>
#include "Gameplay/RifleState.h"
#include <limits>

TEST_CASE("Rifle cadence does not depend on presentation frame rate") {
  for(int fps:{10,15,30,60,144}) {
    gameplay::RifleState w;
    int shots=0;
    for(int i=0;i<fps;++i)shots+=w.tick(1.f/fps,true);
    CHECK(shots==10);CHECK(w.magazine==20);CHECK(w.shotsFired==10);
  }
}
TEST_CASE("Rifle cannot bank idle time or fire more ammo than available") {
  gameplay::RifleState w;
  for(int i=0;i<200;++i)CHECK(w.tick(.1f,false)==0);
  CHECK(w.tick(.5f,true)==1);
  for(int i=0;i<100;++i)w.tick(.05f,true);
  CHECK(w.magazine==0);CHECK(w.shotsFired==30);
  CHECK(w.tick(.5f,true)==0);
}
TEST_CASE("Semi auto fires once per trigger press") {
  gameplay::RifleState w;w.automatic=false;
  CHECK(w.tick(.02f,true)==1);
  for(int i=0;i<100;++i)CHECK(w.tick(.02f,true)==0);
  w.tick(.1f,false);CHECK(w.tick(.02f,true)==1);
  CHECK(w.magazine==28);
}
TEST_CASE("Reload transfers ammunition once at the end and conserves rounds") {
  gameplay::RifleState w;w.magazine=11;w.reserve=8;
  CHECK(w.reload());CHECK_FALSE(w.reload());
  for(int i=0;i<25;++i)CHECK(w.tick(.1f,true)==0);
  CHECK(w.magazine==11);CHECK(w.reserve==8);CHECK(w.reloading());
  CHECK(w.tick(.1f,true)==0);
  CHECK(w.magazine==19);CHECK(w.reserve==0);CHECK_FALSE(w.reloading());
  CHECK_FALSE(w.reload());
  CHECK(w.tick(.1f,true)==1);
}
TEST_CASE("Paused, disabled and invalid ticks do not simulate shooting or reload") {
  gameplay::RifleState w;w.magazine=0;w.reload();
  const auto remaining=w.reloadRemaining;
  CHECK(w.tick(5.f,true,false)==0);CHECK(w.reloadRemaining==remaining);
  w.enabled=false;CHECK(w.tick(.1f,true)==0);CHECK(w.reloadRemaining==remaining);
  w.enabled=true;
  CHECK(w.tick(std::numeric_limits<float>::quiet_NaN(),true)==0);
  CHECK(w.tick(-1.f,true)==0);CHECK(w.reloadRemaining==remaining);
  for(int i=0;i<33;++i)w.tick(.1f,false);
  CHECK(w.magazine==30);CHECK(w.reserve==90);CHECK_FALSE(w.reloading());
  CHECK_FALSE(w.reload());
}

#include "Scene/Scene.h"
#include "ECS/Components.h"
#include "Gameplay/PlayerSpawn.h"
TEST_CASE("Rifle loadout survives a scene save and reload") {
  Scene scene;auto &reg=scene.registry();
  const auto player=reg.create();reg.emplace<TransformComponent>(player);
  reg.emplace<NameComponent>(player,NameComponent("Armed player"));
  auto &w=reg.emplace<RifleComponent>(player).state;
  reg.emplace<RigidbodyComponent>(player).lockRotation=true;
  w.magazine=7;w.reserve=53;w.automatic=false;
  const auto saved=scene.serializeToString();
  REQUIRE(scene.loadFromString(saved));
  int count=0;
  for(auto id:reg.view<RifleComponent>()) {
    ++count;const auto &loaded=reg.get<RifleComponent>(id).state;
    CHECK(loaded.magazine==7);CHECK(loaded.reserve==53);
    CHECK_FALSE(loaded.automatic);CHECK(loaded.enabled);CHECK_FALSE(loaded.reloading());
    CHECK(reg.get<RigidbodyComponent>(id).lockRotation);
  }
  CHECK(count==1);
}

TEST_CASE("Legacy player capsule is repaired without altering a valid player") {
  TransformComponent tr; tr.scale={2.007f,5.224f,1};
  ColliderComponent col;col.shape=ColliderComponent::Shape::Capsule;
  col.dimensions={.6f,1.8f,.6f};col.offset={0,10,0};
  RigidbodyComponent rb;
  CHECK(gameplay::repairPlayerCapsule(tr,col,rb));
  CHECK(tr.scale==glm::vec3(1));CHECK(rb.lockRotation);
  CHECK(gameplay::playerEyeHeight(tr,col)==doctest::Approx(1.62f));
  CHECK_FALSE(gameplay::repairPlayerCapsule(tr,col,rb));
}
TEST_CASE("Terrain spawn recovers stale maps and water while keeping authored positions") {
  TerrainSettings ts;ts.authoredWoodland=true;ts.worldRadius=520;
  auto height=[](glm::vec2 p){return p.y<0?-2.f:3.f;};
  auto water=[](glm::vec2){return 0.f;};
  auto stale=gameplay::terrainPlayerSpawn({-113,-104,1479},{0,12},ts,1.62f,height,water);
  CHECK(stale==glm::vec3(0,4.62f,12));
  auto pond=gameplay::terrainPlayerSpawn({0,1,-30},{0,-30},ts,1.62f,height,water);
  CHECK(pond==glm::vec3(0,4.62f,12));
  auto valid=gameplay::terrainPlayerSpawn({80,6,50},{0,12},ts,1.62f,height,water);
  CHECK(valid==glm::vec3(80,6,50));
  auto below=gameplay::terrainPlayerSpawn({80,-15,50},{0,12},ts,1.62f,height,water);
  CHECK(below.y==doctest::Approx(4.62f));
}
